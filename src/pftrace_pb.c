#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "pb_writer.h"
#include "pftrace_internal.h"

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

/* Drains the ring as protobuf packets. Caller holds out_lock. */
int pb_drain_locked(struct pf_trace *t)
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
