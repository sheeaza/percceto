#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

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
bool intern_lookup_or_insert(struct intern_table *tab, const char *key,
                             uint64_t *iid_out, bool *is_new_out)
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
        pb_put_bytes(b, zeros,
                     sizeof(zeros) < n - b->len ? sizeof(zeros) : n - b->len);
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
        pb_buf_init(&t->raw.tracks);
        pb_buf_init(&t->raw.events);
        pb_buf_warm(&t->raw.names, PF_NAME_WARM + 64);
        pb_buf_warm(&t->raw.tracks, 1024);
        pb_buf_warm(&t->raw.events, PF_OUT_FLUSH_THRESHOLD + PF_PAYLOAD_BYTES);
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
    atomic_fetch_add_explicit(&t->bytes_written, t->out.len,
                              memory_order_relaxed);
    pb_buf_reset(&t->out);
    return 0;
}

/* Drains the ring to the output file. Caller holds out_lock. */
static int drain_locked(struct pf_trace *t)
{
    if (t->cfg.format == PF_FORMAT_RAW)
        return raw_drain_locked(t);
    return pb_drain_locked(t);
}

/* --- Drain thread -------------------------------------------------------
 * Runs in both modes, but does different work. Continuous: write the ring
 * out whenever it crosses the watermark or the interval elapses.
 * Snapshot: never write, only advance the ring's tail so it keeps a
 * bounded history on a known record boundary -- without that, producers
 * lap the reader's starting point and a dump has nothing it can walk. */

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
    while (!t->stop &&
           atomic_load_explicit(&t->wake_pending, memory_order_acquire) == 0) {
        if (pthread_cond_timedwait(&t->drain_cv, &t->drain_lock, &deadline) ==
            ETIMEDOUT)
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

        if (t->cfg.mode == PF_MODE_CONTINUOUS) {
            pthread_mutex_lock(&t->out_lock);
            (void)drain_locked(t); /* errno surfaces via pf_trace_dump */
            pthread_mutex_unlock(&t->out_lock);
        }
        if (stopping)
            break;
    }
    return NULL;
}

/* Nudges the drain thread once the ring passes the watermark.
 *
 * Continuous mode only. Snapshot mode needs no wake for correctness:
 * pf_ring_reserve keeps @tail on a recoverable boundary itself, as part of
 * the same atomic step that moves @head, so there is no window for a
 * producer to slip past a decision made out here. Nothing on the hot path
 * has to prompt it.
 *
 * The exchange on wake_pending means a burst of events past the watermark
 * costs one futex between drains, not one per event. */
static void maybe_wake_drain(struct pf_trace *t)
{
    if (t->cfg.mode != PF_MODE_CONTINUOUS)
        return;

    uint64_t occupied = pf_ring_occupancy_slots(&t->ring);
    if (occupied < t->watermark_slots)
        return;

    if (atomic_exchange_explicit(&t->wake_pending, 1, memory_order_acq_rel) !=
        0)
        return; /* a wake is already pending; no second futex */

    pthread_mutex_lock(&t->drain_lock);
    pthread_cond_signal(&t->drain_cv);
    pthread_mutex_unlock(&t->drain_lock);
}

/* --- Event emission (hot path) ------------------------------------------
 * Reserve a slot, fill it, submit it. No allocation and no I/O, in any
 * backend.
 *
 * Whether it takes a lock is the backend's business, and the one thing
 * they genuinely differ on. The ringbuf_mpsc ring takes none:
 * concurrent producers own disjoint slots, so their writes proceed in
 * parallel and only the claim itself is serialized, by one atomic. The
 * ringbuf_zcopy ring must serialize claims with a mutex -- held across the
 * claim alone, never the fill or the submit -- which is the cost that
 * backend exists to measure. See BENCHMARK_ring_backends.md.
 *
 * Four stores and no string work at all: the name is stored by address,
 * and the drain thread is what eventually reads the characters. */

static void emit_event(struct pf_trace *t, uint32_t type, uint64_t track_uuid,
                       const char *name, bool has_value, uint64_t raw_value,
                       bool value_is_double)
{
    /* Timestamp before reserving, not after. The reservation can spin
     * briefly when producers collide, and a timestamp taken afterwards
     * would include that delay -- attributing contention in the tracer to
     * the code being traced. Taken first, it is the caller's moment, and
     * the cost of the reservation lands between this event and the next
     * rather than inside this one. Events can then reach the ring in a
     * different order than their timestamps, which is fine: Perfetto sorts
     * by timestamp. */
    uint64_t ts = now_ns();

    struct pf_rec_res res;
    if (!pf_ring_reserve(&t->ring, &res))
        return; /* no space, or a peer mid-fill; counted as dropped */

    struct pf_rec *r = (struct pf_rec *)res.rec;
    r->ts = ts;
    r->track = track_uuid;
    /* Mutually exclusive by construction -- see the layout comment. */
    if (has_value)
        r->u.value = raw_value;
    else
        r->u.name = name;
    r->type = type;
    r->flags = (has_value ? RF_HAS_VALUE : 0u) |
               (value_is_double ? RF_VALUE_IS_DOUBLE : 0u);

    pf_ring_submit(&t->ring, &res);
    maybe_wake_drain(t);
}

void pf_slice_begin_impl(struct pf_trace *t, uint64_t track_uuid,
                         const char *name)
{
    emit_event(t, TYPE_SLICE_BEGIN, track_uuid, name, false, 0, false);
}

void pf_instant_event_impl(struct pf_trace *t, uint64_t track_uuid,
                           const char *name)
{
    emit_event(t, TYPE_INSTANT, track_uuid, name, false, 0, false);
}

void pf_slice_end(struct pf_trace *t, uint64_t track_uuid)
{
    emit_event(t, TYPE_SLICE_END, track_uuid, NULL, false, 0, false);
}

void pf_counter_set_int(struct pf_trace *t, uint64_t counter_track_uuid,
                        int64_t value)
{
    emit_event(t, TYPE_COUNTER, counter_track_uuid, NULL, true, (uint64_t)value,
               false);
}

void pf_counter_set_double(struct pf_trace *t, uint64_t counter_track_uuid,
                           double value)
{
    uint64_t bits;
    memcpy(&bits, &value, 8);
    emit_event(t, TYPE_COUNTER, counter_track_uuid, NULL, true, bits, true);
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
static uint64_t track_add_locked(struct pf_trace *t, enum track_kind kind,
                                 uint64_t parent_uuid, int32_t pid, int32_t tid,
                                 const char *name, enum pf_unit unit)
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

static uint64_t track_add(struct pf_trace *t, enum track_kind kind,
                          uint64_t parent_uuid, int32_t pid, int32_t tid,
                          const char *name, enum pf_unit unit)
{
    pthread_mutex_lock(&t->track_lock);
    uint64_t uuid =
        track_add_locked(t, kind, parent_uuid, pid, tid, name, unit);
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

bool track_emit_due(struct pf_trace *t, unsigned i, unsigned first,
                    struct track_def *out)
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

uint64_t pf_track_thread(struct pf_trace *t, int32_t pid, int32_t tid,
                         const char *name)
{
    return track_add(t, TRACK_THREAD, t->process_uuid, pid, tid, name,
                     PF_UNIT_UNSPECIFIED);
}

/* Whether @td's stored name is @name. Compares against the truncated form
 * stored by set_track_name(), so a name longer than the cap matches its own
 * stored copy. */
static bool track_name_eq(const struct track_def *td, const char *name)
{
    size_t n = strnlen(name, PF_TRACK_NAME_MAX - 1);
    return td->has_name && strlen(td->name) == n &&
           memcmp(td->name, name, n) == 0;
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
        ok = t->tracks[i].kind == TRACK_PROCESS ||
             t->tracks[i].kind == TRACK_THREAD;
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
static uint64_t track_find_or_add(struct pf_trace *t, enum track_kind kind,
                                  uint64_t parent_uuid, int32_t pid,
                                  int32_t tid, const char *name,
                                  enum pf_unit unit)
{
    pthread_mutex_lock(&t->track_lock);
    uint64_t uuid = 0;
    bool found = false;
    for (unsigned i = 0; i < t->track_count && name; i++) {
        const struct track_def *td = &t->tracks[i];
        if (td->kind != kind || td->parent_uuid != parent_uuid ||
            !track_name_eq(td, name))
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

uint64_t pf_track_counter(struct pf_trace *t, uint64_t parent_uuid,
                          const char *name, enum pf_unit unit)
{
    /* 0 selects the process track. Any other parent must be a process or
     * thread track, not a counter. */
    if (parent_uuid && !track_can_parent(t, parent_uuid))
        return 0;
    uint64_t parent = parent_uuid ? parent_uuid : t->process_uuid;
    return track_find_or_add(t, TRACK_COUNTER, parent, 0, 0, name, unit);
}

uint64_t pf_track_thread_named(struct pf_trace *t, int32_t pid, int32_t tid,
                               const char *name)
{
    /* pid and tid are recorded only when the track is created. */
    return track_find_or_add(t, TRACK_THREAD, t->process_uuid, pid, tid, name,
                             PF_UNIT_UNSPECIFIED);
}

/* --- Simple API -------------------------------------------------------
 * The trace is the process default, set by pf_trace_open() and cleared by
 * pf_trace_close(). It is expected to be created once by the main thread
 * and live for the whole process, so it is never swapped while threads
 * use it. Each thread caches the uuid of its own track in a single TLS
 * slot. The hot path is one pointer load, one TLS read and a flag check.
 * Syscalls (gettid, getpid) run once per thread, on first use.
 *
 * Renames are kept in the slot, so a name set before registration applies
 * at registration, and one set after re-emits the descriptor. */

static struct pf_trace *g_default_trace;

struct thread_slot {
    bool registered;
    uint64_t uuid;
    bool has_name;
    char name[PF_TRACK_NAME_MAX];
};

static __thread struct thread_slot tl_slot;

static struct pf_trace *default_trace(void)
{
    return g_default_trace;
}

/* Cache miss: register this thread's track on @t. Name precedence: the
 * name set by pf_thread_name(), then the OS thread name, then "thread-<tid>". */
static uint64_t thread_track_register(struct pf_trace *t)
{
    char os_name[PF_TRACK_NAME_MAX];
    char fallback[32];
    const char *name = tl_slot.has_name ? tl_slot.name : NULL;
    int32_t tid = (int32_t)gettid();

    if (!name &&
        pthread_getname_np(pthread_self(), os_name, sizeof(os_name)) == 0 &&
        os_name[0])
        name = os_name;
    if (!name) {
        snprintf(fallback, sizeof(fallback), "thread-%d", tid);
        name = fallback;
    }

    tl_slot.uuid = track_add(t, TRACK_THREAD, t->process_uuid,
                             (int32_t)getpid(), tid, name, PF_UNIT_UNSPECIFIED);
    tl_slot.registered = true;
    return tl_slot.uuid;
}

static uint64_t thread_track(struct pf_trace *t)
{
    if (tl_slot.registered)
        return tl_slot.uuid;
    return thread_track_register(t);
}

void pf_slice_begin_sp_impl(const char *name)
{
    struct pf_trace *t = default_trace();
    if (t)
        pf_slice_begin_impl(t, thread_track(t), name);
}

void pf_slice_end_sp(void)
{
    struct pf_trace *t = default_trace();
    if (t)
        pf_slice_end(t, thread_track(t));
}

void pf_instant_event_sp_impl(const char *name)
{
    struct pf_trace *t = default_trace();
    if (t)
        pf_instant_event_impl(t, thread_track(t), name);
}

void pf_thread_name(const char *name)
{
    if (!name)
        return;
    size_t n = strnlen(name, PF_TRACK_NAME_MAX - 1);
    memcpy(tl_slot.name, name, n);
    tl_slot.name[n] = '\0';
    tl_slot.has_name = true;

    struct pf_trace *t = default_trace();
    if (t && tl_slot.registered)
        track_rename(t, tl_slot.uuid, tl_slot.name);
}

uint64_t pf_thread_track_sp(void)
{
    struct pf_trace *t = default_trace();
    return t ? thread_track(t) : 0;
}

uint64_t pf_track_counter_sp(uint64_t parent_uuid, const char *name,
                             enum pf_unit unit)
{
    struct pf_trace *t = default_trace();
    return t ? pf_track_counter(t, parent_uuid, name, unit) : 0;
}

void pf_counter_set_int_sp(uint64_t counter_track_uuid, int64_t value)
{
    struct pf_trace *t = default_trace();
    if (t)
        pf_counter_set_int(t, counter_track_uuid, value);
}

void pf_counter_set_double_sp(uint64_t counter_track_uuid, double value)
{
    struct pf_trace *t = default_trace();
    if (t)
        pf_counter_set_double(t, counter_track_uuid, value);
}

/* --- Lifecycle --------------------------------------------------------- */

void pf_config_init(struct pf_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->ring_bytes = 8u * 1024u * 1024u;
    cfg->mode = PF_MODE_SNAPSHOT;
    cfg->format = PF_FORMAT_PROTOBUF;
    cfg->watermark_pct = 75;
    cfg->drain_interval_ms = 100;
    cfg->max_tracks = 256;
}

struct pf_trace *pf_trace_open(const struct pf_config *cfg)
{
    if (!cfg || !cfg->path || cfg->watermark_pct > 99) {
        errno = EINVAL;
        return NULL;
    }

    struct pf_trace *t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;

    t->fd = -1;
    t->cfg = *cfg;
    if (t->cfg.ring_bytes == 0)
        t->cfg.ring_bytes = 8u * 1024u * 1024u;
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

    if (pf_ring_init(&t->ring, PF_PAYLOAD_BYTES,
                     t->cfg.ring_bytes / PF_PAYLOAD_BYTES,
                     t->cfg.mode == PF_MODE_SNAPSHOT) != 0)
        goto free_tracks;

    /* The rounded-up capacity, not what was asked for: backends round
     * up to a power of two, a page multiple or a whole number of slots,
     * and a watermark derived from the request would be a different
     * fraction of the ring than it claims to be. */
    size_t slots = pf_ring_capacity_slots(&t->ring);
    t->watermark_slots = slots * t->cfg.watermark_pct / 100u;

    /* O_APPEND so every write lands at the end, which is what lets a
     * continuous trace stay readable while it is still being written. */
    t->fd = open(t->cfg.path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (t->fd < 0)
        goto destroy_ring;

    staging_init(t);

    intern_table_reset(&t->name_table);

    pthread_mutex_init(&t->out_lock, NULL);
    pthread_mutex_init(&t->track_lock, NULL);

    /* The process track is the parent of the simple API's thread tracks.
     * Registered before the drain starts, so its descriptor is in the first
     * dump. Named by the caller, or else after the program's comm. */
    char comm[16] = "";
    if (!t->cfg.process_name)
        prctl(PR_GET_NAME, comm, 0, 0, 0);
    const char *pname = t->cfg.process_name ? t->cfg.process_name :
                        comm[0]             ? comm :
                                              NULL;
    t->process_uuid = track_add(t, TRACK_PROCESS, 0, (int32_t)getpid(), 0,
                                pname, PF_UNIT_UNSPECIFIED);

    pthread_mutex_init(&t->drain_lock, NULL);
    pthread_cond_init(&t->drain_cv, NULL);
    atomic_init(&t->wake_pending, 0);
    atomic_init(&t->bytes_written, 0);

    /* Before the drain thread starts, so the header is the file's first
     * bytes. An existing file is refused here, see raw_write_header. */
    if (t->cfg.format == PF_FORMAT_RAW && raw_write_header(t) != 0)
        goto destroy_sync;

    if (pthread_create(&t->drain_thread, NULL, drain_thread_fn, t) != 0)
        goto destroy_sync;
    t->drain_thread_started = true;
    g_default_trace = t;
    return t;

destroy_sync:
    pthread_cond_destroy(&t->drain_cv);
    pthread_mutex_destroy(&t->drain_lock);
    pthread_mutex_destroy(&t->track_lock);
    pthread_mutex_destroy(&t->out_lock);
    staging_free(t);
    close(t->fd);
destroy_ring:
    pf_ring_destroy(&t->ring);
free_tracks:
    free(t->tracks);
free_trace:
    free(t);
    return NULL;
}

int pf_trace_dump(struct pf_trace *t)
{
    pthread_mutex_lock(&t->out_lock);
    int rc = drain_locked(t);
    pthread_mutex_unlock(&t->out_lock);
    return rc;
}

int pf_trace_flush(struct pf_trace *t)
{
    if (pf_trace_dump(t) != 0)
        return -1;
    if (fdatasync(t->fd) != 0)
        return -1;
    return 0;
}

void pf_trace_stats(const struct pf_trace *t, struct pf_stats *out)
{
    /* Casting away const to read atomics: these are counters, and
     * reading them does not modify the trace in any observable way. */
    struct pf_trace *m = (struct pf_trace *)t;
    pf_ring_stats(&m->ring, &out->events, &out->dropped);
    out->bytes_written =
        atomic_load_explicit(&m->bytes_written, memory_order_relaxed);
}

void pf_trace_close(struct pf_trace *t)
{
    if (!t)
        return;

    /* Before the drain stops, so no simple-API call reaches a trace that
     * is being torn down. */
    if (g_default_trace == t)
        g_default_trace = NULL;

    if (t->drain_thread_started) {
        pthread_mutex_lock(&t->drain_lock);
        t->stop = true;
        pthread_cond_signal(&t->drain_cv);
        pthread_mutex_unlock(&t->drain_lock);
        pthread_join(t->drain_thread, NULL);
    }

    /* Write whatever is still buffered, so a trace exists even if the
     * program never asked for one explicitly. */
    pthread_mutex_lock(&t->out_lock);
    (void)drain_locked(t);
    pthread_mutex_unlock(&t->out_lock);

    if (t->fd >= 0)
        close(t->fd);
    staging_free(t);
    pf_ring_destroy(&t->ring);
    free(t->tracks);

    pthread_cond_destroy(&t->drain_cv);
    pthread_mutex_destroy(&t->drain_lock);
    pthread_mutex_destroy(&t->track_lock);
    pthread_mutex_destroy(&t->out_lock);
    free(t);
}
