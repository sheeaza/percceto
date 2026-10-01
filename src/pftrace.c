#include "pftrace.h"
#include "pb_writer.h"
#include "pf_ring.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* --- Confirmed Perfetto wire-format field numbers -------------------
 * Pulled directly from the compiled perfetto_trace_pb2 DESCRIPTOR (the
 * official schema), not from memory. See plan doc for the full table. */

/* Trace */
#define FN_TRACE_PACKET 1

/* TracePacket */
#define FN_PACKET_TIMESTAMP 8
#define FN_PACKET_TRUSTED_SEQ_ID 10
#define FN_PACKET_TRACK_EVENT 11
#define FN_PACKET_INTERNED_DATA 12
#define FN_PACKET_SEQUENCE_FLAGS 13
#define FN_PACKET_TRACK_DESCRIPTOR 60

#define SEQ_INCREMENTAL_STATE_CLEARED 1u
#define SEQ_NEEDS_INCREMENTAL_STATE 2u

/* TrackEvent */
#define FN_EVENT_CATEGORY_IIDS 3
#define FN_EVENT_TYPE 9
#define FN_EVENT_NAME_IID 10
#define FN_EVENT_TRACK_UUID 11
#define FN_EVENT_COUNTER_VALUE 30
#define FN_EVENT_CATEGORIES_STR 22
#define FN_EVENT_NAME_STR 23
#define FN_EVENT_DOUBLE_COUNTER_VALUE 44

#define TYPE_SLICE_BEGIN 1
#define TYPE_SLICE_END 2
#define TYPE_INSTANT 3
#define TYPE_COUNTER 4

/* TrackDescriptor */
#define FN_TRACK_UUID 1
#define FN_TRACK_NAME 2
#define FN_TRACK_PROCESS 3
#define FN_TRACK_THREAD 4
#define FN_TRACK_PARENT_UUID 5
#define FN_TRACK_COUNTER 8

/* ThreadDescriptor / ProcessDescriptor */
#define FN_THREAD_PID 1
#define FN_THREAD_TID 2
#define FN_THREAD_NAME 5
#define FN_PROCESS_PID 1
#define FN_PROCESS_NAME 6

/* CounterDescriptor */
#define FN_COUNTER_UNIT 3

/* InternedData / EventName / EventCategory */
#define FN_INTERNED_EVENT_CATEGORIES 1
#define FN_INTERNED_EVENT_NAMES 2
#define FN_IID 1
#define FN_NAME_STR 2

/* --- Ring record layout -------------------------------------------------
 * Byte offsets within a record. The tag is first because pf_ring treats
 * it as the record's publication flag; everything after it is plain
 * bytes written by the owning producer alone.
 *
 * Variable-length by design: a slice_end carries no name and no value and
 * so costs 32 bytes, against the 48 a fixed-stride slot would spend on
 * every event regardless. Strings are copied in raw and converted to iids
 * later by the drain thread, which keeps the intern table (and its
 * hashing) off the hot path entirely. */

#define R_TAG 0 /* 8: lap/len, written last by pf_ring_publish */
#define R_TS 8 /* 8: timestamp, ns */
#define R_TRACK 16 /* 8: track_uuid */
#define R_TYPE 24 /* 1: TYPE_* */
#define R_NLEN 25 /* 1: name bytes following the header */
#define R_CLEN 26 /* 1: category bytes following the name */
#define R_FLAGS 27 /* 1: see RF_* */
#define R_BODY 28 /* then: value (8, counters only), name, category */

#define RF_HAS_VALUE 0x1u
#define RF_VALUE_IS_DOUBLE 0x2u

#define PF_NAME_MAX 255u

/* Cap on a track's stored name. Fixed so the track table is one
 * allocation at open and never grows. */
#define PF_TRACK_NAME_MAX 64

/* Flush the staging buffer once it passes this, so one write() carries
 * many packets instead of one syscall per event. */
#define PF_OUT_FLUSH_THRESHOLD (128u * 1024u)

/* Snapshot mode keeps this fraction of the ring as history, trimming the
 * rest. The remainder is headroom: without it producers would lap the
 * oldest retained record while the trim walk was reading it, and the
 * reader's boundary would be unrecoverable. */
#define PF_SNAPSHOT_RETAIN_NUM 3
#define PF_SNAPSHOT_RETAIN_DEN 4

/* Trim is triggered above the retain point, not at it, so each walk frees
 * a worthwhile span. Waking at exactly the retain point means every trim
 * reclaims the handful of bytes just produced and is re-triggered by the
 * next event, turning an amortized O(1) cost into a walk per event. */
#define PF_SNAPSHOT_WAKE_NUM 7
#define PF_SNAPSHOT_WAKE_DEN 8

static size_t round_up8(size_t v)
{
    return (v + 7u) & ~(size_t)7u;
}

/* --- Interning tables -------------------------------------------------
 * Fixed-capacity open-addressing hash tables (string -> iid). Owned
 * exclusively by whichever thread is draining, serialized by
 * pf_trace.out_lock, so they need no locking of their own. If a table
 * ever fills up, callers fall back to inline string fields instead of
 * iids -- always correct, just less deduplicated. */

#define PF_INTERN_CAP 1021 /* prime */

struct intern_slot {
    char key[PF_NAME_MAX + 1]; /* empty string = free slot */
    uint64_t iid;
};

struct intern_table {
    struct intern_slot slots[PF_INTERN_CAP];
    size_t count;
    uint64_t next_iid;
};

static uint64_t fnv1a_len(const char *s, size_t len)
{
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* Returns true if `key` is now represented by *iid_out (found or newly
 * inserted); *is_new_out tells the caller whether this is a fresh
 * entry that still needs an InternedData record emitted. Returns false
 * only when the table is full and `key` wasn't already present. */
static bool intern_lookup_or_insert(struct intern_table *tab, const char *key,
                                    size_t len, uint64_t *iid_out,
                                    bool *is_new_out)
{
    size_t idx = (size_t)(fnv1a_len(key, len) % PF_INTERN_CAP);
    for (size_t probe = 0; probe < PF_INTERN_CAP; probe++) {
        struct intern_slot *slot = &tab->slots[idx];
        if (slot->key[0] == '\0') {
            if (tab->count >= PF_INTERN_CAP)
                return false;
            memcpy(slot->key, key, len);
            slot->key[len] = '\0';
            slot->iid = tab->next_iid++;
            tab->count++;
            *iid_out = slot->iid;
            *is_new_out = true;
            return true;
        }
        if (strlen(slot->key) == len && memcmp(slot->key, key, len) == 0) {
            *iid_out = slot->iid;
            *is_new_out = false;
            return true;
        }
        idx = (idx + 1) % PF_INTERN_CAP;
    }
    return false;
}

static void intern_table_reset(struct intern_table *tab)
{
    memset(tab, 0, sizeof(*tab));
    tab->next_iid = 1;
}

/* --- Track table --------------------------------------------------------
 * Track descriptors cannot live in the ring: it overwrites its oldest
 * records, so a descriptor emitted at startup would be long gone by the
 * time a dump happened, leaving events referring to unknown track_uuids.
 * They are kept here instead and re-emitted whenever a dump needs them. */

enum track_kind {
    TRACK_PROCESS,
    TRACK_THREAD,
    TRACK_COUNTER,
};

struct track_def {
    enum track_kind kind;
    uint64_t uuid;
    uint64_t parent_uuid;
    int32_t pid;
    int32_t tid;
    enum pf_unit unit;
    bool has_name;
    char name[PF_TRACK_NAME_MAX];
};

/* --- Trace context ---------------------------------------------------- */

struct pf_trace {
    int fd;
    struct pf_config cfg;
    struct pf_ring ring;

    size_t watermark_bytes;
    /* Ring occupancy at which a producer nudges the drain thread. The
     * watermark in continuous mode; the trim point in snapshot mode. */
    size_t wake_threshold_bytes;
    /* Snapshot mode: how much history a trim keeps. Below
     * wake_threshold_bytes, so each trim frees a worthwhile span instead
     * of being re-triggered by the next event. */
    size_t retain_bytes;

    /* Serializes drains: a caller's pf_trace_dump() against the drain
     * thread, and against another caller's. Also covers the intern
     * tables, the staging buffers and tracks_emitted. */
    pthread_mutex_t out_lock;

    /* Guards the track table against concurrent pf_track_* calls, and
     * against a drain reading it. Not a hot-path lock. */
    pthread_mutex_t track_lock;
    struct track_def *tracks;
    unsigned track_count;
    unsigned tracks_emitted;
    uint64_t next_uuid;

    /* Drain thread wakeup. wake_pending is checked by producers without
     * the lock, so one futex is spent per drain cycle rather than per
     * event past the watermark. */
    pthread_t drain_thread;
    bool drain_thread_started;
    pthread_mutex_t drain_lock;
    pthread_cond_t drain_cv;
    _Atomic unsigned wake_pending;
    bool stop;

    /* Reused across dumps so steady-state draining allocates nothing. */
    struct pb_buf out;
    struct pb_buf scratch_a;
    struct pb_buf scratch_b;
    struct pb_buf scratch_c;
    /* Holds one InternedData entry while it is built, before being
     * embedded into scratch_a. Separate so the two never alias. */
    struct pb_buf scratch_entry;
    /* Snapshot mode copies each record out of the ring before encoding it,
     * so a producer overwriting the original mid-encode cannot feed torn
     * bytes into the intern table. Fixed size, so no allocation. */
    uint8_t rec_copy[PF_REC_MAX];

    struct intern_table name_table;
    struct intern_table cat_table;
    bool header_written;

    _Atomic uint64_t bytes_written;
    _Atomic uint64_t truncated_names;
};

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
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

static int out_flush(struct pf_trace *t)
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

/* Frames `packet` as one entry of the (implicit, streamed) Trace.packet
 * repeated field and stages it for writing. */
static int stage_packet(struct pf_trace *t, const struct pb_buf *packet)
{
    pb_put_tag(&t->out, FN_TRACE_PACKET, 2);
    pb_put_varint(&t->out, packet->len);
    pb_put_bytes(&t->out, packet->data, packet->len);
    if (t->out.len >= PF_OUT_FLUSH_THRESHOLD)
        return out_flush(t);
    return 0;
}

static void add_interned_entry(struct pb_buf *interned, uint32_t field_no,
                               uint64_t iid, const char *name, size_t len,
                               struct pb_buf *entry)
{
    pb_buf_reset(entry);
    pb_put_varint_field(entry, FN_IID, iid);
    pb_put_string_field(entry, FN_NAME_STR, name, len);
    pb_put_submessage_field(interned, field_no, entry);
}

/* --- Track descriptor emission ----------------------------------------- */

static int emit_track(struct pf_trace *t, const struct track_def *td,
                      uint32_t extra_flags)
{
    struct pb_buf *inner = &t->scratch_a;
    struct pb_buf *desc = &t->scratch_b;
    struct pb_buf *packet = &t->scratch_c;

    pb_buf_reset(inner);
    pb_buf_reset(desc);
    pb_buf_reset(packet);

    pb_put_varint_field(desc, FN_TRACK_UUID, td->uuid);
    if (td->parent_uuid)
        pb_put_varint_field(desc, FN_TRACK_PARENT_UUID, td->parent_uuid);

    switch (td->kind) {
    case TRACK_PROCESS:
        pb_put_varint_field(inner, FN_PROCESS_PID, (uint64_t)(uint32_t)td->pid);
        if (td->has_name)
            pb_put_string_field(inner, FN_PROCESS_NAME, td->name,
                                strlen(td->name));
        pb_put_submessage_field(desc, FN_TRACK_PROCESS, inner);
        break;
    case TRACK_THREAD:
        pb_put_varint_field(inner, FN_THREAD_PID, (uint64_t)(uint32_t)td->pid);
        pb_put_varint_field(inner, FN_THREAD_TID, (uint64_t)(uint32_t)td->tid);
        if (td->has_name)
            pb_put_string_field(inner, FN_THREAD_NAME, td->name,
                                strlen(td->name));
        pb_put_submessage_field(desc, FN_TRACK_THREAD, inner);
        break;
    case TRACK_COUNTER:
        if (td->unit != PF_UNIT_UNSPECIFIED)
            pb_put_varint_field(inner, FN_COUNTER_UNIT, (uint64_t)td->unit);
        if (td->has_name)
            pb_put_string_field(desc, FN_TRACK_NAME, td->name,
                                strlen(td->name));
        pb_put_submessage_field(desc, FN_TRACK_COUNTER, inner);
        break;
    }

    pb_put_varint_field(packet, FN_PACKET_TIMESTAMP, now_ns());
    pb_put_varint_field(packet, FN_PACKET_TRUSTED_SEQ_ID, 1);
    if (extra_flags)
        pb_put_varint_field(packet, FN_PACKET_SEQUENCE_FLAGS, extra_flags);
    pb_put_submessage_field(packet, FN_PACKET_TRACK_DESCRIPTOR, desc);

    return stage_packet(t, packet);
}

/* Emits whatever track descriptors the output file still needs.
 *
 * `restart` resets the sequence's incremental state, which is what makes
 * a snapshot dump self-contained: interning restarts from scratch and
 * every track is re-declared, so the appended window can be read without
 * reference to anything written earlier. Continuous mode passes false
 * after the first drain and emits only newly registered tracks.
 *
 * Caller holds out_lock. */
static int emit_track_header(struct pf_trace *t, bool restart)
{
    pthread_mutex_lock(&t->track_lock);
    unsigned count = t->track_count;
    pthread_mutex_unlock(&t->track_lock);

    unsigned first = restart ? 0 : t->tracks_emitted;
    if (restart) {
        intern_table_reset(&t->name_table);
        intern_table_reset(&t->cat_table);
    }

    for (unsigned i = first; i < count; i++) {
        /* The flag belongs on the first packet of the restarted
         * sequence; later packets in the same dump must not repeat it. */
        uint32_t flags =
            (restart && i == first) ? SEQ_INCREMENTAL_STATE_CLEARED : 0u;
        if (emit_track(t, &t->tracks[i], flags) != 0)
            return -1;
    }

    /* A restart with no tracks at all still has to clear state, or the
     * reader keeps stale interning from the previous window. */
    if (restart && count == 0) {
        pb_buf_reset(&t->scratch_c);
        pb_put_varint_field(&t->scratch_c, FN_PACKET_TIMESTAMP, now_ns());
        pb_put_varint_field(&t->scratch_c, FN_PACKET_TRUSTED_SEQ_ID, 1);
        pb_put_varint_field(&t->scratch_c, FN_PACKET_SEQUENCE_FLAGS,
                            SEQ_INCREMENTAL_STATE_CLEARED);
        if (stage_packet(t, &t->scratch_c) != 0)
            return -1;
    }

    t->tracks_emitted = count;
    return 0;
}

/* --- Record -> packet -------------------------------------------------- */

/* Caller holds out_lock. */
static int emit_record(struct pf_trace *t, const uint8_t *rec)
{
    uint64_t ts, track_uuid;
    memcpy(&ts, rec + R_TS, 8);
    memcpy(&track_uuid, rec + R_TRACK, 8);
    uint8_t type = rec[R_TYPE];
    uint8_t nlen = rec[R_NLEN];
    uint8_t clen = rec[R_CLEN];
    uint8_t flags = rec[R_FLAGS];

    const uint8_t *body = rec + R_BODY;
    uint64_t raw_value = 0;
    if (flags & RF_HAS_VALUE) {
        memcpy(&raw_value, body, 8);
        body += 8;
    }
    const char *name = (const char *)body;
    const char *category = (const char *)body + nlen;

    struct pb_buf *interned = &t->scratch_a;
    struct pb_buf *ev = &t->scratch_b;
    struct pb_buf *packet = &t->scratch_c;
    struct pb_buf *entry = &t->scratch_entry;
    pb_buf_reset(interned);
    pb_buf_reset(ev);
    pb_buf_reset(packet);

    bool have_interned = false;
    uint64_t name_iid = 0;
    bool name_interned = false;
    if (nlen) {
        bool is_new = false;
        name_interned = intern_lookup_or_insert(&t->name_table, name, nlen,
                                                &name_iid, &is_new);
        if (name_interned && is_new) {
            add_interned_entry(interned, FN_INTERNED_EVENT_NAMES, name_iid,
                               name, nlen, entry);
            have_interned = true;
        }
    }

    uint64_t cat_iid = 0;
    bool cat_interned = false;
    if (clen) {
        bool is_new = false;
        cat_interned = intern_lookup_or_insert(&t->cat_table, category, clen,
                                               &cat_iid, &is_new);
        if (cat_interned && is_new) {
            add_interned_entry(interned, FN_INTERNED_EVENT_CATEGORIES, cat_iid,
                               category, clen, entry);
            have_interned = true;
        }
    }

    pb_put_varint_field(ev, FN_EVENT_TYPE, type);
    pb_put_varint_field(ev, FN_EVENT_TRACK_UUID, track_uuid);
    if (nlen) {
        if (name_interned)
            pb_put_varint_field(ev, FN_EVENT_NAME_IID, name_iid);
        else
            pb_put_string_field(ev, FN_EVENT_NAME_STR, name, nlen);
    }
    if (clen) {
        if (cat_interned)
            pb_put_varint_field(ev, FN_EVENT_CATEGORY_IIDS, cat_iid);
        else
            pb_put_string_field(ev, FN_EVENT_CATEGORIES_STR, category, clen);
    }
    if (flags & RF_HAS_VALUE) {
        if (flags & RF_VALUE_IS_DOUBLE)
            pb_put_fixed64_field(ev, FN_EVENT_DOUBLE_COUNTER_VALUE, raw_value);
        else
            pb_put_varint_field(ev, FN_EVENT_COUNTER_VALUE, raw_value);
    }

    pb_put_varint_field(packet, FN_PACKET_TIMESTAMP, ts);
    pb_put_varint_field(packet, FN_PACKET_TRUSTED_SEQ_ID, 1);
    pb_put_varint_field(packet, FN_PACKET_SEQUENCE_FLAGS,
                        SEQ_NEEDS_INCREMENTAL_STATE);
    if (have_interned)
        pb_put_submessage_field(packet, FN_PACKET_INTERNED_DATA, interned);
    pb_put_submessage_field(packet, FN_PACKET_TRACK_EVENT, ev);

    int rc = stage_packet(t, packet);
    return rc;
}

/* Drains the ring to the output file. Caller holds out_lock. */
static int drain_locked(struct pf_trace *t)
{
    bool restart = !t->header_written || t->cfg.mode == PF_MODE_SNAPSHOT;
    if (emit_track_header(t, restart) != 0)
        return -1;
    t->header_written = true;

    struct pf_ring_reader rd;
    pf_ring_read_begin(&t->ring, &rd);

    const uint8_t *rec;
    uint32_t len;
    while ((rec = pf_ring_next(&t->ring, &rd, &len)) != NULL) {
        uint64_t rec_pos = rd.pos - len;

        /* Snapshot mode has no backpressure, so a producer may be
         * overwriting this record right now. Copy it out before touching
         * it, then verify the source bytes were stable for the whole copy:
         * the lap check in pf_ring_next opened this seqlock and
         * pf_ring_still_valid closes it.
         *
         * Copying first is what makes the rest safe. Encoding straight
         * from the ring would feed a half-overwritten name into the
         * intern table, and since the table outlives the rolled-back
         * packet, a later valid event could reference an iid whose
         * InternedData record was never written -- an unreadable trace.
         * A bounded ring cannot be overwritten under the reader, so it
         * skips the copy entirely and encodes in place. */
        const uint8_t *src = rec;
        if (!t->ring.bounded) {
            memcpy(t->rec_copy, rec, len);
            if (!pf_ring_still_valid(&t->ring, rec_pos)) {
                /* Lapped: the reader's position is no longer a known
                 * record boundary, so nothing after this can be trusted.
                 * Restart from head, keeping what was already staged. */
                pf_ring_resync(&t->ring);
                return out_flush(t);
            }
            src = t->rec_copy;
        }

        if (emit_record(t, src) != 0)
            return -1;
    }

    pf_ring_read_commit(&t->ring, &rd);
    return out_flush(t);
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
        } else {
            pf_ring_trim(&t->ring, t->retain_bytes);
        }

        if (stopping)
            break;
    }
    return NULL;
}

/* Hot path: keep the ring's tail from being lapped, and nudge the drain
 * thread.
 *
 * Both modes need this, for different reasons. Continuous mode must write
 * before the ring fills, or events get dropped. Snapshot mode never
 * writes, but must still advance the tail before producers lap it.
 *
 * Signalling the drain thread is not sufficient on its own for snapshot
 * mode: at ~30ns per event a producer can consume the headroom between
 * the wake threshold and the end of the ring before the woken thread is
 * scheduled, and the reader's boundary is then gone. So past the
 * threshold the producer also trims inline. That costs a tag walk, but
 * only on the rare event that crosses the line, and it is pure pointer
 * arithmetic -- no copying, no syscall, no lock. */
static void maybe_wake_drain(struct pf_trace *t, uint32_t need)
{
    uint64_t head = atomic_load_explicit(&t->ring.head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&t->ring.tail, memory_order_relaxed);
    if (head + need - tail < t->wake_threshold_bytes)
        return;

    /* Snapshot mode: reclaim here and now rather than hoping to be
     * scheduled in time. Continuous mode must not do this -- its tail
     * marks what has been written to disk, and moving it would discard
     * events instead of saving them. */
    if (t->cfg.mode == PF_MODE_SNAPSHOT)
        pf_ring_trim(&t->ring, t->retain_bytes);

    if (atomic_exchange_explicit(&t->wake_pending, 1, memory_order_acq_rel) !=
        0)
        return; /* a wake is already pending; no second futex */

    pthread_mutex_lock(&t->drain_lock);
    pthread_cond_signal(&t->drain_cv);
    pthread_mutex_unlock(&t->drain_lock);
}

/* --- Event emission (hot path) ------------------------------------------
 * Claim a byte range, fill it, publish it. No allocation, no I/O, and no
 * lock: concurrent producers own disjoint ranges, so their copies proceed
 * in parallel and only the claim itself is serialized, by one atomic. */

static size_t clamp_len(struct pf_trace *t, const char *s, size_t *len_out)
{
    if (!s) {
        *len_out = 0;
        return 0;
    }
    size_t n = strnlen(s, PF_NAME_MAX + 1);
    if (n > PF_NAME_MAX) {
        n = PF_NAME_MAX;
        atomic_fetch_add_explicit(&t->truncated_names, 1, memory_order_relaxed);
    }
    *len_out = n;
    return n;
}

static void emit_event(struct pf_trace *t, uint8_t type, uint64_t track_uuid,
                       const char *name, const char *category, bool has_value,
                       uint64_t raw_value, bool value_is_double)
{
    size_t nlen, clen;
    clamp_len(t, name, &nlen);
    clamp_len(t, category, &clen);

    uint32_t need =
        (uint32_t)round_up8(R_BODY + (has_value ? 8u : 0u) + nlen + clen);

    /* Timestamp before claiming, not after. The claim can spin briefly
     * when producers collide, and a timestamp taken afterwards would
     * include that delay -- attributing contention in the tracer to the
     * code being traced. Taken first, it is the caller's moment, and the
     * cost of the claim lands between this event and the next rather than
     * inside this one. Events can then reach the ring in a different order
     * than their timestamps, which is fine: Perfetto sorts by timestamp. */
    uint64_t ts = now_ns();

    uint64_t off;
    uint8_t *rec = pf_ring_claim(&t->ring, need, &off);
    if (!rec)
        return; /* bounded mode, reader behind; counted as dropped */

    memcpy(rec + R_TS, &ts, 8);
    memcpy(rec + R_TRACK, &track_uuid, 8);
    rec[R_TYPE] = type;
    rec[R_NLEN] = (uint8_t)nlen;
    rec[R_CLEN] = (uint8_t)clen;
    rec[R_FLAGS] = (uint8_t)((has_value ? RF_HAS_VALUE : 0u) |
                             (value_is_double ? RF_VALUE_IS_DOUBLE : 0u));

    uint8_t *body = rec + R_BODY;
    if (has_value) {
        memcpy(body, &raw_value, 8);
        body += 8;
    }
    if (nlen)
        memcpy(body, name, nlen);
    if (clen)
        memcpy(body + nlen, category, clen);

    pf_ring_publish(&t->ring, off, need);
    maybe_wake_drain(t, need);
}

void pf_slice_begin(struct pf_trace *t, uint64_t track_uuid, const char *name,
                    const char *category)
{
    emit_event(t, TYPE_SLICE_BEGIN, track_uuid, name, category, false, 0,
               false);
}

void pf_instant_event(struct pf_trace *t, uint64_t track_uuid, const char *name,
                      const char *category)
{
    emit_event(t, TYPE_INSTANT, track_uuid, name, category, false, 0, false);
}

void pf_slice_end(struct pf_trace *t, uint64_t track_uuid)
{
    emit_event(t, TYPE_SLICE_END, track_uuid, NULL, NULL, false, 0, false);
}

void pf_counter_set_int(struct pf_trace *t, uint64_t counter_track_uuid,
                        int64_t value)
{
    emit_event(t, TYPE_COUNTER, counter_track_uuid, NULL, NULL, true,
               (uint64_t)value, false);
}

void pf_counter_set_double(struct pf_trace *t, uint64_t counter_track_uuid,
                           double value)
{
    uint64_t bits;
    memcpy(&bits, &value, 8);
    emit_event(t, TYPE_COUNTER, counter_track_uuid, NULL, NULL, true, bits,
               true);
}

/* --- Tracks ------------------------------------------------------------ */

static uint64_t track_add(struct pf_trace *t, enum track_kind kind,
                          uint64_t parent_uuid, int32_t pid, int32_t tid,
                          const char *name, enum pf_unit unit)
{
    pthread_mutex_lock(&t->track_lock);
    if (t->track_count >= t->cfg.max_tracks) {
        pthread_mutex_unlock(&t->track_lock);
        return 0;
    }
    struct track_def *td = &t->tracks[t->track_count++];
    memset(td, 0, sizeof(*td));
    td->kind = kind;
    td->uuid = t->next_uuid++;
    td->parent_uuid = parent_uuid;
    td->pid = pid;
    td->tid = tid;
    td->unit = unit;
    if (name) {
        size_t n = strnlen(name, PF_TRACK_NAME_MAX - 1);
        memcpy(td->name, name, n);
        td->name[n] = '\0';
        td->has_name = true;
    }
    uint64_t uuid = td->uuid;
    pthread_mutex_unlock(&t->track_lock);
    return uuid;
}

uint64_t pf_track_process(struct pf_trace *t, int32_t pid, const char *name)
{
    return track_add(t, TRACK_PROCESS, 0, pid, 0, name, PF_UNIT_UNSPECIFIED);
}

uint64_t pf_track_thread(struct pf_trace *t, uint64_t parent_uuid, int32_t pid,
                         int32_t tid, const char *name)
{
    return track_add(t, TRACK_THREAD, parent_uuid, pid, tid, name,
                     PF_UNIT_UNSPECIFIED);
}

uint64_t pf_track_counter(struct pf_trace *t, uint64_t parent_uuid,
                          const char *name, enum pf_unit unit)
{
    return track_add(t, TRACK_COUNTER, parent_uuid, 0, 0, name, unit);
}

/* --- Lifecycle --------------------------------------------------------- */

void pf_config_init(struct pf_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->ring_bytes = 8u * 1024u * 1024u;
    cfg->mode = PF_MODE_SNAPSHOT;
    cfg->watermark_pct = 75;
    cfg->drain_interval_ms = 100;
    cfg->max_tracks = 256;
    cfg->single_producer = false;
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
        goto fail;

    if (pf_ring_init(&t->ring, t->cfg.ring_bytes,
                     t->cfg.mode == PF_MODE_CONTINUOUS,
                     t->cfg.single_producer) != 0)
        goto fail;

    t->watermark_bytes = t->ring.cap / 100u * t->cfg.watermark_pct;
    t->wake_threshold_bytes =
        t->cfg.mode == PF_MODE_CONTINUOUS ?
            t->watermark_bytes :
            t->ring.cap / PF_SNAPSHOT_WAKE_DEN * PF_SNAPSHOT_WAKE_NUM;
    t->retain_bytes =
        t->ring.cap / PF_SNAPSHOT_RETAIN_DEN * PF_SNAPSHOT_RETAIN_NUM;

    /* O_APPEND so every write lands at the end, which is what lets a
     * continuous trace stay readable while it is still being written. */
    t->fd = open(t->cfg.path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (t->fd < 0)
        goto fail;

    pb_buf_init(&t->out);
    pb_buf_init(&t->scratch_a);
    pb_buf_init(&t->scratch_b);
    pb_buf_init(&t->scratch_c);
    pb_buf_init(&t->scratch_entry);
    pb_buf_warm(&t->out, PF_OUT_FLUSH_THRESHOLD + PF_REC_MAX + 1024);
    pb_buf_warm(&t->scratch_a, PF_REC_MAX + 256);
    pb_buf_warm(&t->scratch_b, PF_REC_MAX + 256);
    pb_buf_warm(&t->scratch_c, PF_REC_MAX + 512);
    pb_buf_warm(&t->scratch_entry, PF_NAME_MAX + 64);

    intern_table_reset(&t->name_table);
    intern_table_reset(&t->cat_table);

    pthread_mutex_init(&t->out_lock, NULL);
    pthread_mutex_init(&t->track_lock, NULL);
    pthread_mutex_init(&t->drain_lock, NULL);
    pthread_cond_init(&t->drain_cv, NULL);
    atomic_init(&t->wake_pending, 0);
    atomic_init(&t->bytes_written, 0);
    atomic_init(&t->truncated_names, 0);

    if (pthread_create(&t->drain_thread, NULL, drain_thread_fn, t) != 0) {
        pthread_cond_destroy(&t->drain_cv);
        pthread_mutex_destroy(&t->drain_lock);
        pthread_mutex_destroy(&t->track_lock);
        pthread_mutex_destroy(&t->out_lock);
        goto fail;
    }
    t->drain_thread_started = true;
    return t;

fail: {
    int saved = errno;
    if (t->fd >= 0)
        close(t->fd);
    pb_buf_free(&t->out);
    pb_buf_free(&t->scratch_a);
    pb_buf_free(&t->scratch_b);
    pb_buf_free(&t->scratch_c);
    pf_ring_destroy(&t->ring);
    free(t->tracks);
    free(t);
    errno = saved;
    return NULL;
}
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
    out->events =
        atomic_load_explicit(&m->ring.published, memory_order_relaxed);
    out->bytes_written =
        atomic_load_explicit(&m->bytes_written, memory_order_relaxed);
    out->dropped = atomic_load_explicit(&m->ring.dropped, memory_order_relaxed);
    out->truncated_names =
        atomic_load_explicit(&m->truncated_names, memory_order_relaxed);
    out->resyncs = atomic_load_explicit(&m->ring.resyncs, memory_order_relaxed);
}

void pf_trace_close(struct pf_trace *t)
{
    if (!t)
        return;

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
    pb_buf_free(&t->out);
    pb_buf_free(&t->scratch_a);
    pb_buf_free(&t->scratch_b);
    pb_buf_free(&t->scratch_c);
    pb_buf_free(&t->scratch_entry);
    pf_ring_destroy(&t->ring);
    free(t->tracks);

    pthread_cond_destroy(&t->drain_cv);
    pthread_mutex_destroy(&t->drain_lock);
    pthread_mutex_destroy(&t->track_lock);
    pthread_mutex_destroy(&t->out_lock);
    free(t);
}
