/* Trace lifecycle: configuration, open, stats, and close. */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "pftrace_internal.h"

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
        pb_buf_warm(&t->pb.scratch_a, PF_PACKET_WARM + 256);
        pb_buf_warm(&t->pb.scratch_b, PF_PACKET_WARM + 256);
        pb_buf_warm(&t->pb.scratch_c, PF_PACKET_WARM + 512);
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
    }
}

/* --- Lifecycle --------------------------------------------------------- */

/* Defaults for the pf_config fields that take 0 to mean "the default". */
#define DEFAULT_BUFFER_BYTES (8u * 1024u * 1024u)
#define DEFAULT_WATERMARK_PCT 75
#define DEFAULT_DRAIN_INTERVAL_MS 100
#define DEFAULT_MAX_TRACKS 256

void pf_config_init(struct pf_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->buffer_bytes = DEFAULT_BUFFER_BYTES;
    cfg->chunk_slots = 256;
    cfg->max_threads = 256;
    cfg->mode = PF_MODE_SNAPSHOT;
    cfg->format = PF_FORMAT_PROTOBUF;
    cfg->watermark_pct = DEFAULT_WATERMARK_PCT;
    cfg->drain_interval_ms = DEFAULT_DRAIN_INTERVAL_MS;
    cfg->max_tracks = DEFAULT_MAX_TRACKS;
    cfg->clock_id = CLOCK_BOOTTIME;
}

/* The clocks a trace can be stamped with. Each one is a builtin clock in
 * ClockSnapshot, so Perfetto can name it as the primary trace clock. */
static bool clock_ok(clockid_t clk)
{
    return clk == CLOCK_REALTIME || clk == CLOCK_MONOTONIC || clk == CLOCK_BOOTTIME;
}

/* Whether pf_trace_open can take @cfg. */
static bool config_ok(const struct pf_config *cfg)
{
    if (!cfg || !cfg->path || !open_name_ok(cfg) || !clock_ok(cfg->clock_id))
        return false;
    return cfg->watermark_pct <= 99 && cfg->chunk_slots != 0 && cfg->max_threads != 0;
}

struct pf_trace *pf_trace_open(const struct pf_config *cfg)
{
    if (!config_ok(cfg)) {
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
        t->cfg.buffer_bytes = DEFAULT_BUFFER_BYTES;
    if (t->cfg.watermark_pct == 0)
        t->cfg.watermark_pct = DEFAULT_WATERMARK_PCT;
    if (t->cfg.drain_interval_ms == 0)
        t->cfg.drain_interval_ms = DEFAULT_DRAIN_INTERVAL_MS;
    if (t->cfg.max_tracks == 0)
        t->cfg.max_tracks = DEFAULT_MAX_TRACKS;
    /* Rotation is continuous only, like the watermark and drain interval. */
    if (t->cfg.mode != PF_MODE_CONTINUOUS || t->cfg.max_file_bytes == 0) {
        t->cfg.max_file_bytes = 0;
        t->cfg.max_files = 0;
    }
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
        /* An existing file keeps its bytes, so the size limit counts them. */
        struct stat st;
        if (fstat(t->fd, &st) == 0)
            t->file_bytes = (uint64_t)st.st_size;
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
    atomic_init(&t->files, t->cfg.mode == PF_MODE_CONTINUOUS ? 1 : 0);
    atomic_init(&t->files_overwritten, 0);
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

    if (t->cfg.mode == PF_MODE_CONTINUOUS && pf_drain_start(t) != 0) {
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

void pf_trace_stats(const struct pf_trace *t, struct pf_stats *out)
{
    /* Casting away const to take the lock and read the counters: reading them
     * does not modify the trace in any observable way. */
    struct pf_trace *m = (struct pf_trace *)t;
    out->events = pf_writers_events(m);
    out->dropped =
        pf_writers_dropped(m) + atomic_load_explicit(&m->overflow_dropped, memory_order_relaxed);
    out->truncated = atomic_load_explicit(&m->truncated, memory_order_relaxed);
    out->bytes_written = atomic_load_explicit(&m->bytes_written, memory_order_relaxed);
    out->files = atomic_load_explicit(&m->files, memory_order_relaxed);
    out->files_overwritten = atomic_load_explicit(&m->files_overwritten, memory_order_relaxed);
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
    if (t->cfg.mode == PF_MODE_CONTINUOUS)
        pf_drain_stop(t);

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
