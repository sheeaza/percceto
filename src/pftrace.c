#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include "pf_encode.h"
#include "pftrace_internal.h"

/* --- Interning table --------------------------------------------------
 * Fixed-capacity open-addressing hash table (name -> iid). Owned
 * exclusively by whichever thread is draining, serialized by
 * pf_trace.out_lock, so it needs no locking of its own. If it ever fills
 * up, callers fall back to inline string fields instead of iids -- always
 * correct, just less deduplicated.
 *
 * Keyed on the name's *address*, not its bytes. Names are string literals
 * (pf_slice_begin enforces it), so one call site always presents the same
 * pointer and a hit is a single comparison with no hashing of characters.
 * Two identical literals in different translation units may not coalesce,
 * which costs a duplicate iid for the same text -- harmless, and the
 * Perfetto UI shows both as the same name. */

/* Fibonacci hashing, to mix an address whose low bits are often
 * predictable (literals land adjacent in .rodata) into the whole index. */
static uint64_t hash_ptr(const char *p)
{
    return (uint64_t)(uintptr_t)p * 11400714819323198485ULL >> 32;
}

/* Returns true if `key` is now represented by *iid_out (found or newly
 * inserted); *is_new_out tells the caller whether this is a fresh
 * entry that still needs an InternedData record emitted. Returns false
 * only when the table is full and `key` wasn't already present. */
bool intern_lookup_or_insert(struct intern_table *tab, const char *key, uint64_t *iid_out,
                             bool *is_new_out)
{
    size_t idx = (size_t)(hash_ptr(key) % PF_INTERN_CAP);
    for (size_t probe = 0; probe < PF_INTERN_CAP; probe++) {
        struct intern_slot *slot = &tab->slots[idx];
        if (!slot->key) {
            if (tab->count >= PF_INTERN_CAP)
                return false;
            slot->key = key;
            slot->iid = tab->next_iid++;
            tab->count++;
            *iid_out = slot->iid;
            *is_new_out = true;
            return true;
        }
        if (slot->key == key) {
            *iid_out = slot->iid;
            *is_new_out = false;
            return true;
        }
        idx = (idx + 1) % PF_INTERN_CAP;
    }
    return false;
}

void intern_table_reset(struct intern_table *tab)
{
    memset(tab, 0, sizeof(*tab));
    tab->next_iid = 1;
}

/* Grow a buffer to `n` bytes of capacity up front, then empty it, so the
 * steady state never reallocates. pb_buf_reset keeps the capacity. */
static void pb_buf_warm(struct pb_buf *b, size_t n)
{
    static const uint8_t zeros[1024] = { 0 };
    while (b->len < n)
        pb_put_bytes(b, zeros, sizeof(zeros) < n - b->len ? sizeof(zeros) : n - b->len);
    pb_buf_reset(b);
}

/* Allocates the staging buffers the trace's format uses. The other format's
 * buffers stay empty, so a trace allocates only what it writes. */
static void staging_init(struct pf_trace *t)
{
    pb_buf_init(&t->out);
    pb_buf_warm(&t->out, PF_OUT_FLUSH_THRESHOLD + PF_PACKET_WARM + 1024);

    if (t->cfg.format == PF_FORMAT_RAW) {
        pb_buf_init(&t->raw.names);
        pb_buf_init(&t->raw.keys);
        pb_buf_init(&t->raw.tracks);
        pb_buf_init(&t->raw.events);
        pb_buf_warm(&t->raw.names, PF_NAME_WARM + 64);
        pb_buf_warm(&t->raw.tracks, 1024);
        pb_buf_warm(&t->raw.events, PF_OUT_FLUSH_THRESHOLD + PF_REC_HEAD_BYTES);
    } else {
        pb_buf_init(&t->pb.scratch_a);
        pb_buf_init(&t->pb.scratch_b);
        pb_buf_init(&t->pb.scratch_c);
        pb_buf_init(&t->pb.scratch_entry);
        pb_buf_warm(&t->pb.scratch_a, PF_PACKET_WARM + 256);
        pb_buf_warm(&t->pb.scratch_b, PF_PACKET_WARM + 256);
        pb_buf_warm(&t->pb.scratch_c, PF_PACKET_WARM + 512);
        pb_buf_warm(&t->pb.scratch_entry, PF_NAME_WARM + 64);
    }
}

/* Frees what staging_init allocated. Safe on a trace that failed midway,
 * since pb_buf_free accepts a buffer that was never filled. */
static void staging_free(struct pf_trace *t)
{
    pb_buf_free(&t->out);
    if (t->cfg.format == PF_FORMAT_RAW) {
        pb_buf_free(&t->raw.names);
        pb_buf_free(&t->raw.keys);
        pb_buf_free(&t->raw.tracks);
        pb_buf_free(&t->raw.events);
    } else {
        pb_buf_free(&t->pb.scratch_a);
        pb_buf_free(&t->pb.scratch_b);
        pb_buf_free(&t->pb.scratch_c);
        pb_buf_free(&t->pb.scratch_entry);
    }
}

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
    pb_buf_reset(&t->out);
    return 0;
}

/* Writes the committed chunks to the output file, in commit order. Caller
 * holds out_lock.
 *
 * Continuous: the committed FIFO is detached, so producers keep committing
 * while it is written, and its chunks go back to the pool afterwards.
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

/* The name a caller passes to pf_trace_open or a dump must match the mode:
 * continuous names its one file at open and dumps take none; snapshot names
 * each dump and takes no name at open. */
static bool open_name_ok(const struct pf_config *cfg)
{
    return cfg->mode == PF_MODE_CONTINUOUS ? leaf_name_ok(cfg->name) : cfg->name == NULL;
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
    if (rc == 0)
        name_out(file, size, leaf);
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

/* --- Event emission (hot path) ------------------------------------------
 * Write one record into the calling thread's chunk. No shared state is read
 * or written here, and the only branch that is not the common case is the
 * rotation once per chunk. See docs/chunk_buffers_design.md.
 *
 * A static name is stored by address, and the drain reads its characters
 * later. A dynamic name is copied into the record now, since the caller's
 * buffer may be reused before the drain runs. */

static void emit_event_args(struct pf_trace *t, uint32_t type, uint64_t track_uuid,
                            const char *name, bool dynamic, bool has_value, uint64_t raw_value,
                            bool value_is_double, unsigned nargs, const struct pf_arg *args)
{
    /* Timestamp before anything else, so the cost of finding a chunk lands
     * between this event and the next rather than inside this one. Chunks
     * are per thread, so records reach the drain in commit order, which is
     * not timestamp order across threads. Perfetto sorts by timestamp. */
    uint64_t ts = now_ns(t->cfg.clock_id);

    struct pf_writer *w = pf_writer_for(t);
    if (!w) {
        atomic_fetch_add_explicit(&t->dropped, 1, memory_order_relaxed);
        return;
    }
    /* Protobuf records are encoded here, on the producer. */
    if (t->cfg.format == PF_FORMAT_PROTOBUF) {
        pf_pb_emit(t, w, ts, type, track_uuid, name, dynamic, has_value, raw_value, value_is_double,
                   nargs, args);
        return;
    }

    /* Raw records carry their arguments in a section after the head. */
    if (nargs > PF_MAX_ARGS)
        nargs = PF_MAX_ARGS;
    uint32_t args_bytes = nargs ? PF_REC_ARGS_HEAD + nargs * PF_REC_ARG_BYTES : 0;

    /* A dynamic name is copied after the head and any arguments, capped at
     * PF_DYN_NAME_MAX. */
    uint32_t len = 0;
    bool copy = dynamic && name && !has_value;
    bool cut = false;
    if (copy) {
        len = (uint32_t)pf_dyn_name_fit(name, &cut);
        if (cut)
            atomic_fetch_add_explicit(&t->truncated, 1, memory_order_relaxed);
    }
    uint32_t padded = (len + PF_REC_ALIGN - 1) & ~(PF_REC_ALIGN - 1);
    uint32_t rec_size = PF_REC_HEAD_BYTES + args_bytes + (copy ? padded : 0);

    /* A record that does not fit the rest of the chunk goes to the next one.
     * Records are never split; see TODO item 3. A record larger than a whole
     * chunk payload cannot be stored at all, so it is dropped and counted. */
    if (rec_size > t->chunk_payload) {
        atomic_fetch_add_explicit(&t->dropped, 1, memory_order_relaxed);
        return;
    }
    if (w->used + rec_size > t->chunk_payload && !pf_writer_rotate(t, w))
        return; /* no chunk free; pf_writer_rotate counted the drop */

    struct pf_rec *r = (struct pf_rec *)(w->cur->bytes + w->used);
    r->ts = ts;
    r->track = track_uuid;
    /* Mutually exclusive by construction -- see the layout comment. */
    if (has_value)
        r->u.value = raw_value;
    else if (copy)
        r->u.name_len = len;
    else
        r->u.name = name;
    r->type = type;
    r->flags = (has_value ? RF_HAS_VALUE : 0u) | (value_is_double ? RF_VALUE_IS_DOUBLE : 0u) |
               (copy ? RF_NAME_COPIED : 0u) | (nargs ? RF_HAS_ARGS : 0u);
    if (nargs) {
        /* The count, 4 reserved bytes, then one entry per argument: the key's
         * pointer, its kind, 4 reserved bytes, and the value's 8 bytes. Keys are
         * literals, so the pointer is what the drain interns. */
        uint32_t *hdr = (uint32_t *)((char *)r + PF_REC_HEAD_BYTES);
        hdr[0] = nargs;
        hdr[1] = 0;
        for (unsigned i = 0; i < nargs; i++) {
            char *e = (char *)hdr + PF_REC_ARGS_HEAD + i * PF_REC_ARG_BYTES;
            uint64_t key = (uint64_t)(uintptr_t)args[i].key;
            uint32_t kind = (uint32_t)args[i].kind;
            uint32_t zero = 0;
            memcpy(e, &key, sizeof key);
            memcpy(e + 8, &kind, sizeof kind);
            memcpy(e + 12, &zero, sizeof zero);
            memcpy(e + 16, &args[i].v, sizeof args[i].v);
        }
    }
    if (copy) {
        char *body = (char *)r + PF_REC_HEAD_BYTES + args_bytes;
        pf_dyn_name_copy(body, name, len, cut);
        memset(body + len, 0, padded - len);
    }
    w->used += rec_size;

    /* Publish the count after the record is complete. Release pairs with the
     * acquire in pf_walk_enter, so a snapshot never reads a half-written record. */
    atomic_store_explicit(&w->cur->used, w->used, memory_order_release);
    atomic_store_explicit(&w->events, atomic_load_explicit(&w->events, memory_order_relaxed) + 1,
                          memory_order_relaxed);
}

static void emit_event(struct pf_trace *t, uint32_t type, uint64_t track_uuid, const char *name,
                       bool dynamic, bool has_value, uint64_t raw_value, bool value_is_double)
{
    emit_event_args(t, type, track_uuid, name, dynamic, has_value, raw_value, value_is_double, 0,
                    NULL);
}

void pf_slice_begin_args_impl(struct pf_trace *t, uint64_t track_uuid, const char *name,
                              unsigned nargs, const struct pf_arg *args)
{
    emit_event_args(t, TYPE_SLICE_BEGIN, track_uuid, name, false, false, 0, false, nargs, args);
}

void pf_instant_event_args_impl(struct pf_trace *t, uint64_t track_uuid, const char *name,
                                unsigned nargs, const struct pf_arg *args)
{
    emit_event_args(t, TYPE_INSTANT, track_uuid, name, false, false, 0, false, nargs, args);
}

void pf_slice_begin_impl(struct pf_trace *t, uint64_t track_uuid, const char *name)
{
    emit_event(t, TYPE_SLICE_BEGIN, track_uuid, name, false, false, 0, false);
}

void pf_slice_begin_dyn_impl(struct pf_trace *t, uint64_t track_uuid, const char *name)
{
    emit_event(t, TYPE_SLICE_BEGIN, track_uuid, name, true, false, 0, false);
}

/* Longest name the formatter writes. A result longer than this is cut by the
 * dynamic path, which ends it in "..." like any long name. */
#define FMT_NAME_BYTES 256

/* Formats @fmt into @buf and returns @buf. A failed format leaves an empty name. */
static const char *format_name(char *buf, size_t size, const char *fmt, va_list ap)
{
    if (vsnprintf(buf, size, fmt, ap) < 0)
        buf[0] = '\0';
    return buf;
}

void pf_slice_begin_fmt_impl(struct pf_trace *t, uint64_t track_uuid, const char *fmt, ...)
{
    char name[FMT_NAME_BYTES];
    va_list ap;
    va_start(ap, fmt);
    pf_slice_begin_dyn_impl(t, track_uuid, format_name(name, sizeof name, fmt, ap));
    va_end(ap);
}

void pf_instant_event_fmt_impl(struct pf_trace *t, uint64_t track_uuid, const char *fmt, ...)
{
    char name[FMT_NAME_BYTES];
    va_list ap;
    va_start(ap, fmt);
    pf_instant_event_dyn_impl(t, track_uuid, format_name(name, sizeof name, fmt, ap));
    va_end(ap);
}

void pf_slice_begin_g_fmt_impl(const char *fmt, ...)
{
    char name[FMT_NAME_BYTES];
    va_list ap;
    va_start(ap, fmt);
    pf_slice_begin_g_dyn_impl(format_name(name, sizeof name, fmt, ap));
    va_end(ap);
}

void pf_instant_event_g_fmt_impl(const char *fmt, ...)
{
    char name[FMT_NAME_BYTES];
    va_list ap;
    va_start(ap, fmt);
    pf_instant_event_g_dyn_impl(format_name(name, sizeof name, fmt, ap));
    va_end(ap);
}

void pf_instant_event_impl(struct pf_trace *t, uint64_t track_uuid, const char *name)
{
    emit_event(t, TYPE_INSTANT, track_uuid, name, false, false, 0, false);
}

void pf_instant_event_dyn_impl(struct pf_trace *t, uint64_t track_uuid, const char *name)
{
    emit_event(t, TYPE_INSTANT, track_uuid, name, true, false, 0, false);
}

void pf_slice_end(struct pf_trace *t, uint64_t track_uuid)
{
    emit_event(t, TYPE_SLICE_END, track_uuid, NULL, false, false, 0, false);
}

void pf_counter_set_int(struct pf_trace *t, uint64_t counter_track_uuid, int64_t value)
{
    emit_event(t, TYPE_COUNTER, counter_track_uuid, NULL, false, true, (uint64_t)value, false);
}

void pf_counter_set_double(struct pf_trace *t, uint64_t counter_track_uuid, double value)
{
    uint64_t bits;
    memcpy(&bits, &value, 8);
    emit_event(t, TYPE_COUNTER, counter_track_uuid, NULL, false, true, bits, true);
}

/* --- Tracks ------------------------------------------------------------ */

static void set_track_name(struct track_def *td, const char *name)
{
    size_t n = strnlen(name, PF_TRACK_NAME_MAX - 1);
    memcpy(td->name, name, n);
    td->name[n] = '\0';
    td->has_name = true;
}

/* Appends a track. Caller holds track_lock. */
static uint64_t track_add_locked(struct pf_trace *t, enum track_kind kind, uint64_t parent_uuid,
                                 int32_t pid, int32_t tid, const char *name, enum pf_unit unit)
{
    if (t->track_count >= t->cfg.max_tracks)
        return 0;
    struct track_def *td = &t->tracks[t->track_count++];
    memset(td, 0, sizeof(*td));
    td->kind = kind;
    td->uuid = t->next_uuid++;
    td->parent_uuid = parent_uuid;
    td->pid = pid;
    td->tid = tid;
    td->unit = unit;
    if (name)
        set_track_name(td, name);
    return td->uuid;
}

static uint64_t track_add(struct pf_trace *t, enum track_kind kind, uint64_t parent_uuid,
                          int32_t pid, int32_t tid, const char *name, enum pf_unit unit)
{
    pthread_mutex_lock(&t->track_lock);
    uint64_t uuid = track_add_locked(t, kind, parent_uuid, pid, tid, name, unit);
    pthread_mutex_unlock(&t->track_lock);
    return uuid;
}

/* Renames the track @uuid. The new name is picked up by the next drain,
 * which re-emits the descriptor. No-op for an unknown uuid. */
static void track_rename(struct pf_trace *t, uint64_t uuid, const char *name)
{
    pthread_mutex_lock(&t->track_lock);
    for (unsigned i = 0; i < t->track_count; i++) {
        struct track_def *td = &t->tracks[i];
        if (td->uuid != uuid)
            continue;
        set_track_name(td, name);
        td->name_version++;
        break;
    }
    pthread_mutex_unlock(&t->track_lock);
}

bool track_emit_due(struct pf_trace *t, unsigned i, unsigned first, struct track_def *out)
{
    pthread_mutex_lock(&t->track_lock);
    struct track_def *td = &t->tracks[i];
    bool due = i >= first || td->name_version != td->emitted_version;
    if (due) {
        *out = *td;
        td->emitted_version = td->name_version;
    }
    pthread_mutex_unlock(&t->track_lock);
    return due;
}

uint64_t pf_track_thread(struct pf_trace *t, int32_t pid, int32_t tid, const char *name)
{
    return track_add(t, TRACK_THREAD, t->process_uuid, pid, tid, name, PF_UNIT_UNSPECIFIED);
}

/* Whether @td's stored name is @name. Compares against the truncated form
 * stored by set_track_name(), so a name longer than the cap matches its own
 * stored copy. */
static bool track_name_eq(const struct track_def *td, const char *name)
{
    size_t n = strnlen(name, PF_TRACK_NAME_MAX - 1);
    return td->has_name && strlen(td->name) == n && memcmp(td->name, name, n) == 0;
}

/* Whether @uuid names a process or thread track, the only kinds a track may
 * be parented under. Tracks are never removed, so a uuid in range is valid. */
static bool track_can_parent(struct pf_trace *t, uint64_t uuid)
{
    bool ok = false;
    pthread_mutex_lock(&t->track_lock);
    for (unsigned i = 0; i < t->track_count; i++) {
        if (t->tracks[i].uuid != uuid)
            continue;
        ok = t->tracks[i].kind == TRACK_PROCESS || t->tracks[i].kind == TRACK_THREAD;
        break;
    }
    pthread_mutex_unlock(&t->track_lock);
    return ok;
}

/* Returns the uuid of the track of @kind under @parent_uuid named @name,
 * adding one with @pid, @tid and @unit if there is none. A NULL @name never
 * matches, so unnamed tracks are always new. A match with a different @unit
 * returns 0. Lookup and insert share one critical section, so two threads
 * registering the same name cannot both insert it. */
static uint64_t track_find_or_add(struct pf_trace *t, enum track_kind kind, uint64_t parent_uuid,
                                  int32_t pid, int32_t tid, const char *name, enum pf_unit unit)
{
    pthread_mutex_lock(&t->track_lock);
    uint64_t uuid = 0;
    bool found = false;
    for (unsigned i = 0; i < t->track_count && name; i++) {
        const struct track_def *td = &t->tracks[i];
        if (td->kind != kind || td->parent_uuid != parent_uuid || !track_name_eq(td, name))
            continue;
        /* Same name with a different unit is a conflict, not a match. */
        uuid = td->unit == unit ? td->uuid : 0;
        found = true;
        break;
    }
    if (!found)
        uuid = track_add_locked(t, kind, parent_uuid, pid, tid, name, unit);
    pthread_mutex_unlock(&t->track_lock);
    return uuid;
}

uint64_t pf_track_counter(struct pf_trace *t, uint64_t parent_uuid, const char *name,
                          enum pf_unit unit)
{
    /* 0 selects the process track. Any other parent must be a process or
     * thread track, not a counter. */
    if (parent_uuid && !track_can_parent(t, parent_uuid))
        return 0;
    uint64_t parent = parent_uuid ? parent_uuid : t->process_uuid;
    return track_find_or_add(t, TRACK_COUNTER, parent, 0, 0, name, unit);
}

/* --- Simple API -------------------------------------------------------
 * The trace is the process default, set by pf_trace_open_g() and cleared by
 * pf_trace_close_g(). pf_trace_open() and pf_trace_close() never touch it.
 * It is expected to be created once by the main thread
 * and live for the whole process, so it is never swapped while threads
 * use it. Each thread caches the uuid of its own track in its pf_tls slot for
 * that trace. The hot path is one pointer load, one TLS read and a flag check.
 * Syscalls (gettid, getpid) run once per thread per trace, on first use.
 *
 * Renames are kept in the slot, so a name set before registration applies
 * at registration, and one set after re-emits the descriptor. */

static struct pf_trace *g_default_trace;

static struct pf_trace *default_trace(void)
{
    return g_default_trace;
}

/* Cache miss: register this thread's track on @t. Name precedence: the
 * name set by pf_thread_name(), then the OS thread name, then "thread-<tid>".
 * Binds the thread to @t first, so the track lives in the slot that holds
 * the thread's writer. */
static uint64_t thread_track_register(struct pf_trace *t)
{
    char os_name[PF_TRACK_NAME_MAX];
    char fallback[32];
    const char *name = pf_tls.has_name ? pf_tls.name : NULL;
    int32_t tid = (int32_t)gettid();

    pf_writer_for(t);

    if (!name && pthread_getname_np(pthread_self(), os_name, sizeof(os_name)) == 0 && os_name[0])
        name = os_name;
    if (!name) {
        snprintf(fallback, sizeof(fallback), "thread-%d", tid);
        name = fallback;
    }

    struct pf_tls_slot *s = &pf_tls.slots[t->index];
    s->track_uuid = track_add(t, TRACK_THREAD, t->process_uuid, (int32_t)getpid(), tid, name,
                              PF_UNIT_UNSPECIFIED);
    return s->track_uuid;
}

static uint64_t thread_track(struct pf_trace *t)
{
    struct pf_tls_slot *s = &pf_tls.slots[t->index];
    if (s->trace == t && s->track_uuid)
        return s->track_uuid;
    return thread_track_register(t);
}

void pf_slice_begin_g_impl(const char *name)
{
    struct pf_trace *t = default_trace();
    if (t)
        pf_slice_begin_impl(t, thread_track(t), name);
}

void pf_slice_end_g(void)
{
    struct pf_trace *t = default_trace();
    if (t)
        pf_slice_end(t, thread_track(t));
}

void pf_instant_event_g_impl(const char *name)
{
    struct pf_trace *t = default_trace();
    if (t)
        pf_instant_event_impl(t, thread_track(t), name);
}

void pf_slice_begin_args_g_impl(const char *name, unsigned nargs, const struct pf_arg *args)
{
    struct pf_trace *t = default_trace();
    if (t)
        pf_slice_begin_args_impl(t, thread_track(t), name, nargs, args);
}

void pf_instant_event_args_g_impl(const char *name, unsigned nargs, const struct pf_arg *args)
{
    struct pf_trace *t = default_trace();
    if (t)
        pf_instant_event_args_impl(t, thread_track(t), name, nargs, args);
}

void pf_slice_begin_g_dyn_impl(const char *name)
{
    struct pf_trace *t = default_trace();
    if (t)
        pf_slice_begin_dyn_impl(t, thread_track(t), name);
}

void pf_instant_event_g_dyn_impl(const char *name)
{
    struct pf_trace *t = default_trace();
    if (t)
        pf_instant_event_dyn_impl(t, thread_track(t), name);
}

void pf_thread_name(const char *name)
{
    if (!name)
        return;
    size_t n = strnlen(name, PF_TRACK_NAME_MAX - 1);
    memcpy(pf_tls.name, name, n);
    pf_tls.name[n] = '\0';
    pf_tls.has_name = true;

    struct pf_trace *t = default_trace();
    if (!t)
        return;
    struct pf_tls_slot *s = &pf_tls.slots[t->index];
    if (s->trace == t && s->track_uuid)
        track_rename(t, s->track_uuid, pf_tls.name);
}

uint64_t pf_track_thread_uuid_g(void)
{
    struct pf_trace *t = default_trace();
    return t ? thread_track(t) : 0;
}

uint64_t pf_track_counter_g(uint64_t parent_uuid, const char *name, enum pf_unit unit)
{
    struct pf_trace *t = default_trace();
    return t ? pf_track_counter(t, parent_uuid, name, unit) : 0;
}

void pf_counter_set_int_g(uint64_t counter_track_uuid, int64_t value)
{
    struct pf_trace *t = default_trace();
    if (t)
        pf_counter_set_int(t, counter_track_uuid, value);
}

void pf_counter_set_double_g(uint64_t counter_track_uuid, double value)
{
    struct pf_trace *t = default_trace();
    if (t)
        pf_counter_set_double(t, counter_track_uuid, value);
}

int pf_trace_dump_g(const char *name, char *file, size_t size)
{
    struct pf_trace *t = default_trace();
    if (!t) {
        errno = ENODEV;
        return -1;
    }
    return pf_trace_dump(t, name, file, size);
}

int pf_trace_flush_g(const char *name, char *file, size_t size)
{
    struct pf_trace *t = default_trace();
    if (!t) {
        errno = ENODEV;
        return -1;
    }
    return pf_trace_flush(t, name, file, size);
}

int pf_trace_open_g(const struct pf_config *cfg)
{
    struct pf_trace *t = pf_trace_open(cfg);
    if (!t)
        return -1;
    g_default_trace = t;
    return 0;
}

void pf_trace_close_g(void)
{
    struct pf_trace *t = default_trace();
    if (!t)
        return;

    /* Cleared before the drain stops, so no simple-API call reaches a trace
     * that is being torn down. */
    g_default_trace = NULL;
    pf_trace_close(t);
}

struct pf_trace *pf_trace_default_g(void)
{
    return default_trace();
}

/* --- Lifecycle --------------------------------------------------------- */

void pf_config_init(struct pf_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->buffer_bytes = 8u * 1024u * 1024u;
    cfg->chunk_slots = 256;
    cfg->max_threads = 256;
    cfg->mode = PF_MODE_SNAPSHOT;
    cfg->format = PF_FORMAT_PROTOBUF;
    cfg->watermark_pct = 75;
    cfg->drain_interval_ms = 100;
    cfg->max_tracks = 256;
    cfg->clock_id = CLOCK_BOOTTIME;
}

/* The clocks a trace can be stamped with. Each one is a builtin clock in
 * ClockSnapshot, so Perfetto can name it as the primary trace clock. */
static bool clock_ok(clockid_t clk)
{
    return clk == CLOCK_REALTIME || clk == CLOCK_MONOTONIC || clk == CLOCK_BOOTTIME;
}

struct pf_trace *pf_trace_open(const struct pf_config *cfg)
{
    if (!cfg || !cfg->path || cfg->watermark_pct > 99 || !open_name_ok(cfg) ||
        !clock_ok(cfg->clock_id) || cfg->chunk_slots == 0 || cfg->max_threads == 0) {
        errno = EINVAL;
        return NULL;
    }

    struct pf_trace *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;

    t->fd = -1;
    t->dirfd = -1;
    t->cfg = *cfg;
    if (t->cfg.buffer_bytes == 0)
        t->cfg.buffer_bytes = 8u * 1024u * 1024u;
    if (t->cfg.watermark_pct == 0)
        t->cfg.watermark_pct = 75;
    if (t->cfg.drain_interval_ms == 0)
        t->cfg.drain_interval_ms = 100;
    if (t->cfg.max_tracks == 0)
        t->cfg.max_tracks = 256;
    t->next_uuid = 1;

    t->tracks = calloc(t->cfg.max_tracks, sizeof(*t->tracks));
    if (!t->tracks)
        goto free_trace;

    if (pf_pool_init(t) != 0)
        goto free_tracks;

    /* Held for the life of the trace, since every snapshot dump opens its
     * file in this directory. Opening it here means a bad path fails at open. */
    t->dirfd = open(t->cfg.path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (t->dirfd < 0)
        goto destroy_pool;

    /* Continuous only: its one file. O_APPEND so every write lands at the end,
     * which is what lets a continuous trace stay readable while it is still
     * being written. Snapshot files are opened by each dump. */
    if (t->cfg.mode == PF_MODE_CONTINUOUS) {
        snprintf(t->file_name, sizeof(t->file_name), "%s", t->cfg.name);
        t->fd = openat(t->dirfd, t->cfg.name, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (t->fd < 0)
            goto close_dir;
    }

    staging_init(t);

    intern_table_reset(&t->name_table);
    intern_table_reset(&t->key_table);

    pthread_mutex_init(&t->out_lock, NULL);
    pthread_mutex_init(&t->track_lock, NULL);

    /* The process track is the parent of the simple API's thread tracks.
     * Registered before the drain starts, so its descriptor is in the first
     * dump. Named by the caller, or else after the program's comm. */
    char comm[16] = "";
    if (!t->cfg.process_name)
        prctl(PR_GET_NAME, comm, 0, 0, 0);
    const char *pname = t->cfg.process_name ? t->cfg.process_name : comm[0] ? comm : NULL;
    t->process_uuid =
        track_add(t, TRACK_PROCESS, 0, (int32_t)getpid(), 0, pname, PF_UNIT_UNSPECIFIED);

    pthread_mutex_init(&t->drain_lock, NULL);
    pthread_cond_init(&t->drain_cv, NULL);
    atomic_init(&t->wake_pending, 0);
    atomic_init(&t->bytes_written, 0);
    /* The owner's reference. Each bound thread adds one; see pf_trace_unref. */
    atomic_init(&t->refs, 1);

    /* Before the drain thread starts, so the header is the file's first
     * bytes. An existing file is refused here, see raw_write_header. Snapshot
     * files get their header from each dump, so this is continuous only. */
    if (t->cfg.mode == PF_MODE_CONTINUOUS && t->cfg.format == PF_FORMAT_RAW &&
        raw_write_header(t) != 0)
        goto destroy_sync;

    /* Last failure point: once the index is claimed, nothing else can fail
     * before the trace is returned. */
    if (pf_index_acquire(t) != 0)
        goto destroy_sync;

    if (t->cfg.mode == PF_MODE_CONTINUOUS &&
        pthread_create(&t->drain_thread, NULL, drain_thread_fn, t) != 0) {
        pf_index_release(t);
        goto destroy_sync;
    }
    return t;

    /* Only continuous mode gets this far with a file open, so the close of
     * t->fd below never runs for a snapshot trace. */
destroy_sync:
    pthread_cond_destroy(&t->drain_cv);
    pthread_mutex_destroy(&t->drain_lock);
    pthread_mutex_destroy(&t->track_lock);
    pthread_mutex_destroy(&t->out_lock);
    staging_free(t);
    close(t->fd);
close_dir:
    close(t->dirfd);
destroy_pool:
    pf_pool_destroy(t);
free_tracks:
    free(t->tracks);
free_trace:
    free(t);
    return NULL;
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

void pf_trace_stats(const struct pf_trace *t, struct pf_stats *out)
{
    /* Casting away const to take the lock and read the counters: reading them
     * does not modify the trace in any observable way. */
    struct pf_trace *m = (struct pf_trace *)t;
    out->events = pf_writers_events(m);
    out->dropped = atomic_load_explicit(&m->dropped, memory_order_relaxed);
    out->truncated = atomic_load_explicit(&m->truncated, memory_order_relaxed);
    out->bytes_written = atomic_load_explicit(&m->bytes_written, memory_order_relaxed);
    /* Fixed at open, so no lock is needed to read them. */
    out->chunk_bytes = m->chunk_bytes;
    out->chunk_slots = m->chunk_slots;
    out->slot_bytes = PF_REC_HEAD_BYTES;
    out->chunk_count = m->chunk_count;
    out->pool_bytes = (uint64_t)m->chunk_bytes * m->chunk_count;
    out->meta_bytes =
        m->cfg.max_threads * sizeof(*m->writers) + m->cfg.max_tracks * sizeof(*m->tracks);
}

/* Runs once, from the last reference. Nothing can emit into the trace now: a
 * thread holding a reference gives up its writer before it drops it. So the
 * final drain is the last write to the file. */
static void trace_finish(struct pf_trace *t)
{
    if (t->cfg.mode == PF_MODE_CONTINUOUS) {
        pthread_mutex_lock(&t->drain_lock);
        t->stop = true;
        pthread_cond_signal(&t->drain_cv);
        pthread_mutex_unlock(&t->drain_lock);
        pthread_join(t->drain_thread, NULL);

        /* Write whatever is still buffered, so the file holds every event. A
         * snapshot trace writes nothing here: its buffered window goes out only
         * through an explicit pf_trace_dump. */
        pthread_mutex_lock(&t->out_lock);
        (void)drain_locked(t);
        pthread_mutex_unlock(&t->out_lock);
    }

    if (t->fd >= 0)
        close(t->fd);
    close(t->dirfd);
    staging_free(t);
    pf_pool_destroy(t);
    free(t->tracks);

    pthread_cond_destroy(&t->drain_cv);
    pthread_mutex_destroy(&t->drain_lock);
    pthread_mutex_destroy(&t->track_lock);
    pthread_mutex_destroy(&t->out_lock);
    pf_index_release(t);
    free(t);
}

void pf_trace_unref(struct pf_trace *t)
{
    if (atomic_fetch_sub_explicit(&t->refs, 1, memory_order_acq_rel) == 1)
        trace_finish(t);
}

void pf_trace_close(struct pf_trace *t)
{
    if (!t)
        return;

    /* The calling thread gives up its own reference first. A main thread never
     * runs the exit destructor, so it must do this here or the trace never
     * finishes. */
    pf_unbind_thread(t);
    pf_trace_unref(t);
}
