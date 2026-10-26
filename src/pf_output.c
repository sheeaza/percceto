/* Output files and the drain: writing staged bytes, continuous rotation,
 * snapshot dump files, and the continuous drain thread. */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pftrace_internal.h"

/* --- Output ------------------------------------------------------------ */

/* Write every byte, retrying short writes and EINTR. O_APPEND keeps
 * concurrent appends from interleaving at the file level. */
static int write_all(int fd, const uint8_t *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

int out_flush(struct pf_trace *t)
{
    if (t->out.len == 0)
        return 0;
    if (write_all(t->fd, t->out.data, t->out.len) != 0)
        return -1;
    atomic_fetch_add_explicit(&t->bytes_written, t->out.len, memory_order_relaxed);
    t->file_bytes += t->out.len;
    pb_buf_reset(&t->out);
    return 0;
}

/* Leaf name of rotation slot @slot. Slot 0 is the configured name, so a trace
 * with rotation off writes the one file it always did. Open checked that the
 * suffix fits NAME_MAX. */
static void slot_leaf(const struct pf_trace *t, uint32_t slot, char *leaf, size_t size)
{
    if (slot == 0)
        snprintf(leaf, size, "%s", t->cfg.name);
    else
        snprintf(leaf, size, "%s.%u", t->cfg.name, (unsigned)slot);
}

/* Continuous mode: whether the open file would pass cfg.max_file_bytes with
 * @next more bytes. Only a file that already holds data is rotated, so an
 * oversized item gets a file to itself. Caller holds out_lock. */
bool rotate_due(const struct pf_trace *t, size_t next)
{
    return t->cfg.max_file_bytes && t->file_has_data &&
           t->file_bytes + next > t->cfg.max_file_bytes;
}

/* Continuous mode: closes the open file and opens the next, from the slot the
 * index maps to. The new file is self-contained, as a snapshot file is: its
 * header is written, its tracks are declared again, and interning restarts. A
 * reused slot is truncated, which is what loses its older events. Caller holds
 * out_lock, and has written out anything pending for the old file. */
int rotate_locked(struct pf_trace *t)
{
    uint32_t index = t->file_index + 1;
    uint32_t slot = t->cfg.max_files ? index % t->cfg.max_files : index;
    char leaf[NAME_MAX + 1];
    slot_leaf(t, slot, leaf, sizeof(leaf));

    int fd = openat(t->dirfd, leaf, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;

    close(t->fd);
    t->fd = fd;
    t->file_index = index;
    t->file_bytes = 0;
    t->file_has_data = false;
    t->tracks_emitted = 0;
    t->header_written = false;
    intern_table_reset(&t->name_table);
    intern_table_reset(&t->key_table);
    snprintf(t->file_name, sizeof(t->file_name), "%s", leaf);

    atomic_fetch_add_explicit(&t->files, 1, memory_order_relaxed);
    if (t->cfg.max_files && index >= t->cfg.max_files)
        atomic_fetch_add_explicit(&t->files_overwritten, 1, memory_order_relaxed);

    if (t->cfg.format == PF_FORMAT_RAW)
        return raw_write_header(t);
    return 0;
}

/* Writes the committed chunks to the output file, in commit order. Caller
 * holds out_lock.
 *
 * Continuous: the committed FIFO is detached, so producers keep committing
 * while it is written, and its chunks go back to the pool afterwards. The
 * writers rotate the file between chunks or records when it is full.
 * Snapshot: the FIFO is walked in place under the pool lock, so producers that
 * need a chunk wait for the dump to finish. Nothing is taken from it. */
static int drain_locked(struct pf_trace *t)
{
    int (*write_chunks)(struct pf_trace *, const struct pf_chunk *) =
        t->cfg.format == PF_FORMAT_RAW ? raw_drain_locked : pb_drain_locked;

    if (t->cfg.mode == PF_MODE_SNAPSHOT) {
        /* Holds pool_lock for the whole write, so no writer can rotate and
         * recycle a chunk the walk is reading. Partial chunks are walked in
         * place: their owners keep filling them, but only up to the count
         * they published. */
        pthread_mutex_lock(&t->pool_lock);
        int rc = write_chunks(t, pf_link_partials_locked(t));
        pf_unlink_partials_locked(t);
        pthread_mutex_unlock(&t->pool_lock);
        return rc;
    }

    struct pf_chunk *head = pf_committed_take(t);
    int rc = write_chunks(t, head);
    pf_chunks_release(t, head);
    return rc;
}

/* --- Output files ------------------------------------------------------
 * Continuous mode writes one file, held open from pf_trace_open. Snapshot
 * mode writes a new file for each dump, named
 * <name>-<UTC time>-<seq>.pftrace in the trace's directory. */

/* Whether @name can be a single path component in the trace's directory. */
static bool leaf_name_ok(const char *name)
{
    return name && name[0] && strlen(name) <= NAME_MAX && !strchr(name, '/') &&
           strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

/* Room a rotated file name needs after the configured name: a dot and up to
 * ten digits for a 32-bit slot index. */
#define ROTATE_SUFFIX_MAX 12

/* The name a caller passes to pf_trace_open or a dump must match the mode:
 * continuous names its one file at open and dumps take none; snapshot names
 * each dump and takes no name at open. A rotating trace also needs room for
 * the rotated names' suffix within NAME_MAX. */
bool open_name_ok(const struct pf_config *cfg)
{
    if (cfg->mode != PF_MODE_CONTINUOUS)
        return cfg->name == NULL;
    if (!leaf_name_ok(cfg->name))
        return false;
    return !cfg->max_file_bytes || strlen(cfg->name) + ROTATE_SUFFIX_MAX <= NAME_MAX;
}

static bool dump_name_ok(const struct pf_trace *t, const char *name)
{
    return t->cfg.mode == PF_MODE_SNAPSHOT ? leaf_name_ok(name) : name == NULL;
}

/* Fails with ENAMETOOLONG if the caller wants the name back and @file cannot
 * hold @leaf and its NUL. Checked before any write, so nothing is written
 * that the caller cannot find. */
static int name_fits(const char *leaf, char *file, size_t size)
{
    if (file && strlen(leaf) >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

/* Copies @leaf out to the caller, if they asked for it. Caller has checked
 * that it fits with name_fits. */
static void name_out(char *file, size_t size, const char *leaf)
{
    if (file)
        snprintf(file, size, "%s", leaf);
}

/* Builds the file name for one snapshot dump, <name>-<timestamp>-<seq>.pftrace.
 * The timestamp is UTC to the millisecond, as in 20261018T143007.512Z. The
 * sequence number is per trace and separates dumps that land in the same
 * millisecond. Takes the next sequence number, so the caller holds out_lock.
 * Uses no allocation. */
static int snapshot_leaf(struct pf_trace *t, const char *name, char *leaf, size_t size)
{
    struct timespec ts;
    struct tm tm;
    char stamp[32];

    clock_gettime(CLOCK_REALTIME, &ts);
    if (!gmtime_r(&ts.tv_sec, &tm)) {
        errno = EOVERFLOW;
        return -1;
    }
    strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%S", &tm);

    int len = snprintf(leaf, size, "%s-%s.%03ldZ-%04u.pftrace", name, stamp, ts.tv_nsec / 1000000L,
                       (unsigned)t->dump_seq++);
    if (len < 0 || (size_t)len >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

/* Writes the committed chunks into a new file named @name in the trace's directory,
 * replacing any file already there. The file is self-contained: interning
 * restarts and tracks are re-declared, and a raw file gets its own header.
 * Caller holds out_lock. */
static int snapshot_write_locked(struct pf_trace *t, const char *name, char *file, size_t size,
                                 bool sync)
{
    char leaf[512];
    if (snapshot_leaf(t, name, leaf, sizeof(leaf)) != 0)
        return -1;
    if (name_fits(leaf, file, size) != 0)
        return -1;

    int fd = openat(t->dirfd, leaf, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;

    t->fd = fd;
    t->tracks_emitted = 0;
    intern_table_reset(&t->name_table);
    intern_table_reset(&t->key_table);

    int rc = 0;
    if (t->cfg.format == PF_FORMAT_RAW)
        rc = raw_write_header(t);
    if (rc == 0)
        rc = drain_locked(t);
    if (rc == 0 && sync)
        rc = fdatasync(fd);

    /* Keep the write's errno, not the close's. */
    int err = errno;
    close(fd);
    t->fd = -1;
    errno = err;
    if (rc == 0) {
        name_out(file, size, leaf);
        atomic_fetch_add_explicit(&t->files, 1, memory_order_relaxed);
    }
    return rc;
}

/* Caller holds out_lock. @sync forces the data to disk before returning.
 * Continuous mode reports the name of its open file. */
static int dump_locked(struct pf_trace *t, const char *name, char *file, size_t size, bool sync)
{
    if (t->cfg.mode == PF_MODE_SNAPSHOT)
        return snapshot_write_locked(t, name, file, size, sync);
    if (name_fits(t->file_name, file, size) != 0)
        return -1;
    if (drain_locked(t) != 0)
        return -1;
    if (sync && fdatasync(t->fd) != 0)
        return -1;
    name_out(file, size, t->file_name);
    return 0;
}

/* --- Drain thread -------------------------------------------------------
 * Continuous mode only. It writes the committed chunks out whenever enough
 * have committed to pass the watermark or the interval elapses. Snapshot mode
 * starts no thread: the committed chunks are the window, and a dump is taken
 * on demand by pf_trace_dump. */

/* Waits for a wake signal, the drain interval, or shutdown.
 *
 * Returns true if the trace is shutting down. @stop is read under
 * drain_lock, same as it is written by pf_trace_close, so the shutdown
 * handshake needs no atomic. */
static bool drain_thread_wait(struct pf_trace *t)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    unsigned ms = t->cfg.drain_interval_ms;
    deadline.tv_sec += (time_t)(ms / 1000u);
    deadline.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&t->drain_lock);
    while (!t->stop && atomic_load_explicit(&t->wake_pending, memory_order_acquire) == 0) {
        if (pthread_cond_timedwait(&t->drain_cv, &t->drain_lock, &deadline) == ETIMEDOUT)
            break;
    }
    bool stopping = t->stop;
    pthread_mutex_unlock(&t->drain_lock);
    return stopping;
}

static void *drain_thread_fn(void *arg)
{
    struct pf_trace *t = arg;

    for (;;) {
        bool stopping = drain_thread_wait(t);

        /* Clear before working, so an event arriving mid-drain still
         * schedules the next one rather than being silently coalesced. */
        atomic_store_explicit(&t->wake_pending, 0, memory_order_release);

        pthread_mutex_lock(&t->out_lock);
        (void)drain_locked(t); /* errno surfaces via pf_trace_dump */
        pthread_mutex_unlock(&t->out_lock);
        if (stopping)
            break;
    }
    return NULL;
}

int pf_drain_start(struct pf_trace *t)
{
    return pthread_create(&t->drain_thread, NULL, drain_thread_fn, t) != 0 ? -1 : 0;
}

void pf_drain_stop(struct pf_trace *t)
{
    pthread_mutex_lock(&t->drain_lock);
    t->stop = true;
    pthread_cond_signal(&t->drain_cv);
    pthread_mutex_unlock(&t->drain_lock);
    pthread_join(t->drain_thread, NULL);

    /* Write whatever is still buffered, so the file holds every event. A
     * snapshot trace writes nothing at close: its buffered window goes out
     * only through an explicit pf_trace_dump. */
    pthread_mutex_lock(&t->out_lock);
    (void)drain_locked(t);
    pthread_mutex_unlock(&t->out_lock);
}

/* Wakes the drain thread once committed chunks pass the watermark. Called by
 * a producer that has just committed a chunk, with pool_lock released.
 *
 * The exchange on wake_pending means a burst of commits past the watermark
 * costs one futex between drains, not one per chunk. */
void pf_drain_wake(struct pf_trace *t)
{
    if (t->cfg.mode != PF_MODE_CONTINUOUS)
        return;

    if (atomic_exchange_explicit(&t->wake_pending, 1, memory_order_acq_rel) != 0)
        return; /* a wake is already pending; no second futex */

    pthread_mutex_lock(&t->drain_lock);
    pthread_cond_signal(&t->drain_cv);
    pthread_mutex_unlock(&t->drain_lock);
}

/* Shared by pf_trace_dump and pf_trace_flush. */
static int dump_public(struct pf_trace *t, const char *name, char *file, size_t size, bool sync)
{
    if (!dump_name_ok(t, name)) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&t->out_lock);
    int rc = dump_locked(t, name, file, size, sync);
    pthread_mutex_unlock(&t->out_lock);
    return rc;
}

int pf_trace_dump(struct pf_trace *t, const char *name, char *file, size_t size)
{
    return dump_public(t, name, file, size, false);
}

int pf_trace_flush(struct pf_trace *t, const char *name, char *file, size_t size)
{
    return dump_public(t, name, file, size, true);
}
