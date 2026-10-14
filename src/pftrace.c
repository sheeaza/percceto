#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "pb_writer.h"
#include "pf_ring.h"
#include "pftrace.h"

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
#define FN_EVENT_TYPE 9
#define FN_EVENT_NAME_IID 10
#define FN_EVENT_TRACK_UUID 11
#define FN_EVENT_COUNTER_VALUE 30
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

/* InternedData / EventName */
#define FN_INTERNED_EVENT_NAMES 2
#define FN_IID 1
#define FN_NAME_STR 2

/* --- Ring record layout -------------------------------------------------
 * The payload one event occupies in the ring, written by the owning
 * producer alone. Whatever a backend needs for its own publication
 * bookkeeping sits outside this -- pf_ring_reserve hands back a pointer to
 * the payload, so this struct is the same shape on every backend and the
 * comparison between them is not measuring a difference in record size.
 *
 * Fixed size, which is what lets a backend drop its length bookkeeping
 * entirely -- see pf_ring.h. The name is a borrowed pointer rather than
 * inline bytes: names are string literals, so copying them would spend up
 * to 255 bytes and a memcpy per event to duplicate something immortal.
 * The drain thread dereferences the pointer and interns it, which keeps
 * both the strlen and the hashing off the hot path.
 *
 * @name and @value share eight bytes because no event carries both. A
 * slice or instant has a name and no value; a counter has a value and no
 * name, which is not a shortcut but Perfetto's own rule -- "most other
 * TrackEvent fields (e.g. categories, name, ..) are not supported for
 * TYPE_COUNTER events". RF_HAS_VALUE selects the arm.
 *
 *   0       8       16      24   28   32
 *   +-------+-------+-------+----+----+
 *   | ts    | track | name  |type|flag|
 *   |       |       | value |    |    |
 *   +-------+-------+-------+----+----+
 *
 * Overlaid on the ring rather than copied field by field, which is sound
 * because every member is naturally aligned: a backend hands out 8-byte
 * aligned payloads (PF_REC_ALIGN) and the two 4-byte fields sit in the
 * 24..31 tail. Any buffer this is cast onto must be 8-byte aligned too, as the ring's
 * slots and each backend's pop_buf are.
 */
struct pf_rec {
    uint64_t ts; /* timestamp, ns */
    uint64_t track; /* track_uuid */

    /* Borrowed string literal, or a counter's value; see RF_HAS_VALUE. */
    union {
        const char *name;
        uint64_t value;
    } u;

    uint32_t type; /* TYPE_* */
    uint32_t flags; /* see RF_* */
};

#define RF_HAS_VALUE 0x1u
#define RF_VALUE_IS_DOUBLE 0x2u

/* The layout is the on-ring wire format, so pin it. The size is pinned
 * against the payload every backend carries, which is what the ring's
 * arithmetic assumes; the two must not drift apart. */
_Static_assert(sizeof(struct pf_rec) == PF_PAYLOAD_BYTES,
               "the record is exactly the ring's payload");
_Static_assert(offsetof(struct pf_rec, ts) == 0, "ts leads the record");
_Static_assert(offsetof(struct pf_rec, track) == 8, "track follows ts");
_Static_assert(offsetof(struct pf_rec, u) == 16, "name/value follows track");
_Static_assert(offsetof(struct pf_rec, type) == 24, "type follows name/value");
_Static_assert(offsetof(struct pf_rec, flags) == 28, "no padding before flags");

/* Cap on a track's stored name. Fixed so the track table is one
 * allocation at open and never grows. */
#define PF_TRACK_NAME_MAX 64

/* How much to presize the staging buffers for. These are hints, not
 * limits: an event name is a borrowed literal of any length, so pb_buf
 * grows if one exceeds this. Sized generously enough that the steady state
 * never reallocates -- pb_buf_warm's whole purpose -- while staying an
 * honest typical name rather than a worst case the ring no longer
 * imposes. */
#define PF_NAME_WARM 256u
#define PF_PACKET_WARM (PF_PAYLOAD_BYTES + PF_NAME_WARM)

/* Flush the staging buffer once it passes this, so one write() carries
 * many packets instead of one syscall per event. */
#define PF_OUT_FLUSH_THRESHOLD (128u * 1024u)

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

#define PF_INTERN_CAP 1021 /* prime */

struct intern_slot {
    const char *key; /* NULL = free slot */
    uint64_t iid;
};

struct intern_table {
    struct intern_slot slots[PF_INTERN_CAP];
    size_t count;
    uint64_t next_iid;
};

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
static bool intern_lookup_or_insert(struct intern_table *tab, const char *key,
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

    /* Continuous mode: ring occupancy at which a producer wakes the drain
     * thread, from cfg.watermark_pct. */
    size_t watermark_bytes;

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
    struct intern_table name_table;
    bool header_written;

    _Atomic uint64_t bytes_written;
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
    if (restart)
        intern_table_reset(&t->name_table);

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
    const struct pf_rec *r = (const struct pf_rec *)rec;
    uint64_t ts = r->ts;
    uint64_t track_uuid = r->track;
    uint32_t type = r->type;
    uint32_t flags = r->flags;

    bool has_value = (flags & RF_HAS_VALUE) != 0;
    uint64_t raw_value = has_value ? r->u.value : 0;
    /* Only dereferenced here, on the drain thread -- the hot path stored
     * the address and nothing else. */
    const char *name = has_value ? NULL : r->u.name;
    size_t nlen = name ? strlen(name) : 0;

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
        name_interned =
            intern_lookup_or_insert(&t->name_table, name, &name_iid, &is_new);
        if (name_interned && is_new) {
            add_interned_entry(interned, FN_INTERNED_EVENT_NAMES, name_iid,
                               name, nlen, entry);
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
    if (has_value) {
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
    while ((rec = pf_ring_next(&t->ring, &rd)) != NULL) {
        /* Every backend hands back a record no producer can rewrite while we
         * encode it, so it is encoded in place. */
        if (emit_record(t, rec) != 0)
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
static void maybe_wake_drain(struct pf_trace *t, uint32_t need)
{
    if (t->cfg.mode != PF_MODE_CONTINUOUS)
        return;

    if (pf_ring_occupancy_bytes(&t->ring, need) < t->watermark_bytes)
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

    struct pf_rec_res res = pf_ring_reserve(&t->ring);
    if (!res.rec)
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

    pf_ring_submit(&t->ring, res);
    maybe_wake_drain(t, PF_PAYLOAD_BYTES);
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
    emit_event(t, TYPE_COUNTER, counter_track_uuid, NULL, true,
               (uint64_t)value, false);
}

void pf_counter_set_double(struct pf_trace *t, uint64_t counter_track_uuid,
                           double value)
{
    uint64_t bits;
    memcpy(&bits, &value, 8);
    emit_event(t, TYPE_COUNTER, counter_track_uuid, NULL, true, bits, true);
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
                     t->cfg.mode == PF_MODE_SNAPSHOT) != 0)
        goto fail;

    /* The rounded-up capacity, not what was asked for: backends round
     * up to a power of two, a page multiple or a whole number of slots,
     * and a watermark derived from the request would be a different
     * fraction of the ring than it claims to be. */
    size_t cap = pf_ring_capacity_bytes(&t->ring);
    t->watermark_bytes = cap / 100u * t->cfg.watermark_pct;

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
    pb_buf_warm(&t->out, PF_OUT_FLUSH_THRESHOLD + PF_PACKET_WARM + 1024);
    pb_buf_warm(&t->scratch_a, PF_PACKET_WARM + 256);
    pb_buf_warm(&t->scratch_b, PF_PACKET_WARM + 256);
    pb_buf_warm(&t->scratch_c, PF_PACKET_WARM + 512);
    pb_buf_warm(&t->scratch_entry, PF_NAME_WARM + 64);

    intern_table_reset(&t->name_table);

    pthread_mutex_init(&t->out_lock, NULL);
    pthread_mutex_init(&t->track_lock, NULL);
    pthread_mutex_init(&t->drain_lock, NULL);
    pthread_cond_init(&t->drain_cv, NULL);
    atomic_init(&t->wake_pending, 0);
    atomic_init(&t->bytes_written, 0);

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
    pf_ring_stats(&m->ring, &out->events, &out->dropped);
    out->bytes_written =
        atomic_load_explicit(&m->bytes_written, memory_order_relaxed);
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
