#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "pb_writer.h"
#include "pf_encode.h"
#include "pftrace_internal.h"

/* --- Confirmed Perfetto wire-format field numbers -------------------
 * Pulled directly from the compiled perfetto_trace_pb2 DESCRIPTOR (the
 * official schema), not from memory. The event-side numbers are in
 * pf_encode.h, where the producer encodes them. */

/* TracePacket, for the descriptor and clock packets this file writes */
#define FN_PACKET_CLOCK_SNAPSHOT 6
#define FN_PACKET_TRACK_DESCRIPTOR 60
#define FN_CLOCK_SNAPSHOT_CLOCKS 1
#define FN_CLOCK_SNAPSHOT_PRIMARY 2
#define FN_CLOCK_ID 1
#define FN_CLOCK_TIMESTAMP 2

/* Writes a packet's timestamp with its clock. Perfetto reads a packet with no
 * timestamp_clock_id as BOOTTIME, so a REALTIME or MONOTONIC reading must name
 * its clock or the packet cannot be related to the trace clock. */
static void put_packet_timestamp(struct pb_buf *packet, clockid_t clk, uint64_t ts)
{
    pb_put_varint_field(packet, FN_PACKET_TIMESTAMP, ts);
    pb_put_varint_field(packet, FN_PACKET_TIMESTAMP_CLOCK_ID, builtin_clock(clk));
}

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

/* --- Track descriptor emission ----------------------------------------- */

static int emit_track(struct pf_trace *t, const struct track_def *td, uint32_t extra_flags)
{
    struct pb_buf *inner = &t->pb.scratch_a;
    struct pb_buf *desc = &t->pb.scratch_b;
    struct pb_buf *packet = &t->pb.scratch_c;

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
            pb_put_string_field(inner, FN_PROCESS_NAME, td->name, strlen(td->name));
        pb_put_submessage_field(desc, FN_TRACK_PROCESS, inner);
        break;
    case TRACK_THREAD:
        pb_put_varint_field(inner, FN_THREAD_PID, (uint64_t)(uint32_t)td->pid);
        pb_put_varint_field(inner, FN_THREAD_TID, (uint64_t)(uint32_t)td->tid);
        if (td->has_name)
            pb_put_string_field(inner, FN_THREAD_NAME, td->name, strlen(td->name));
        pb_put_submessage_field(desc, FN_TRACK_THREAD, inner);
        break;
    case TRACK_COUNTER:
        if (td->unit != PF_UNIT_UNSPECIFIED)
            pb_put_varint_field(inner, FN_COUNTER_UNIT, (uint64_t)td->unit);
        if (td->has_name)
            pb_put_string_field(desc, FN_TRACK_NAME, td->name, strlen(td->name));
        pb_put_submessage_field(desc, FN_TRACK_COUNTER, inner);
        break;
    }

    pb_put_varint_field(packet, FN_PACKET_TRUSTED_SEQ_ID, 1);
    if (extra_flags)
        pb_put_varint_field(packet, FN_PACKET_SEQUENCE_FLAGS, extra_flags);
    pb_put_submessage_field(packet, FN_PACKET_TRACK_DESCRIPTOR, desc);

    return stage_packet(t, packet);
}

/* Adds one Clock entry, a builtin id and a reading, to the snapshot. */
static void put_clock_entry(struct pb_buf *snap, struct pb_buf *clock, uint32_t id, uint64_t ts)
{
    pb_buf_reset(clock);
    pb_put_varint_field(clock, FN_CLOCK_ID, id);
    pb_put_varint_field(clock, FN_CLOCK_TIMESTAMP, ts);
    pb_put_submessage_field(snap, FN_CLOCK_SNAPSHOT_CLOCKS, clock);
}

/* Stages a ClockSnapshot naming the trace clock as primary. Its REALTIME
 * reading, taken back to back with the trace clock's, lets Perfetto map the
 * timestamps to wall time. A BOOTTIME reading is added too: trace_processor
 * always links BOOTTIME to the trace clock for a proto trace, and a snapshot
 * with no BOOTTIME entry leaves that link missing and reports it. Caller holds
 * out_lock. */
static int emit_clock_snapshot(struct pf_trace *t)
{
    clockid_t clk = t->cfg.clock_id;
    struct pb_buf *clock = &t->pb.scratch_a;
    struct pb_buf *snap = &t->pb.scratch_b;
    struct pb_buf *packet = &t->pb.scratch_c;

    uint64_t trace_ts = now_ns(clk);
    uint64_t real_ts = now_ns(CLOCK_REALTIME);

    pb_buf_reset(snap);
    pb_buf_reset(packet);
    put_clock_entry(snap, clock, builtin_clock(clk), trace_ts);
    if (clk != CLOCK_REALTIME)
        put_clock_entry(snap, clock, BUILTIN_CLOCK_REALTIME, real_ts);
    if (clk != CLOCK_BOOTTIME)
        put_clock_entry(snap, clock, BUILTIN_CLOCK_BOOTTIME, now_ns(CLOCK_BOOTTIME));
    pb_put_varint_field(snap, FN_CLOCK_SNAPSHOT_PRIMARY, builtin_clock(clk));

    pb_put_varint_field(packet, FN_PACKET_TRUSTED_SEQ_ID, 1);
    pb_put_submessage_field(packet, FN_PACKET_CLOCK_SNAPSHOT, snap);
    return stage_packet(t, packet);
}

/* Emits whatever track descriptors the output file still needs.
 *
 * `restart` clears the state of sequence 1 and re-declares every track, so a
 * snapshot dump's descriptors are self-contained. A restart also writes the
 * ClockSnapshot, so every dump names its own clock. Event interning is not
 * reset here: each chunk defines the names it uses, so a window never depends
 * on an earlier one. Continuous mode passes false after the first drain and
 * emits only newly registered tracks.
 *
 * Caller holds out_lock. */
static int emit_track_header(struct pf_trace *t, bool restart)
{
    pthread_mutex_lock(&t->track_lock);
    unsigned count = t->track_count;
    pthread_mutex_unlock(&t->track_lock);

    unsigned first = restart ? 0 : t->tracks_emitted;
    if (restart) {
        if (emit_clock_snapshot(t) != 0)
            return -1;
    }

    for (unsigned i = 0; i < count; i++) {
        struct track_def td;
        if (!track_emit_due(t, i, first, &td))
            continue;
        /* The flag belongs on the first packet of the restarted
         * sequence; later packets in the same dump must not repeat it. */
        uint32_t flags = (restart && i == first) ? SEQ_INCREMENTAL_STATE_CLEARED : 0u;
        if (emit_track(t, &td, flags) != 0)
            return -1;
    }

    /* A restart with no tracks at all still has to clear state, or the
     * reader keeps stale state for sequence 1 from the previous window. */
    if (restart && count == 0) {
        pb_buf_reset(&t->pb.scratch_c);
        put_packet_timestamp(&t->pb.scratch_c, t->cfg.clock_id, now_ns(t->cfg.clock_id));
        pb_put_varint_field(&t->pb.scratch_c, FN_PACKET_TRUSTED_SEQ_ID, 1);
        pb_put_varint_field(&t->pb.scratch_c, FN_PACKET_SEQUENCE_FLAGS,
                            SEQ_INCREMENTAL_STATE_CLEARED);
        if (stage_packet(t, &t->pb.scratch_c) != 0)
            return -1;
    }

    t->tracks_emitted = count;
    return 0;
}

/* Drains the committed chunks. Each chunk holds whole Trace.packet entries that
 * the producers encoded, so the drain copies its published bytes and does not
 * parse them. Caller holds out_lock. */
int pb_drain_locked(struct pf_trace *t, const struct pf_chunk *head)
{
    bool restart = !t->header_written || t->cfg.mode == PF_MODE_SNAPSHOT;
    if (emit_track_header(t, restart) != 0)
        return -1;
    t->header_written = true;

    for (const struct pf_chunk *c = head; c; c = c->next) {
        uint32_t n = atomic_load_explicit(&c->used, memory_order_acquire);
        pb_put_bytes(&t->out, c->bytes, n);
        if (t->out.len >= PF_OUT_FLUSH_THRESHOLD && out_flush(t) != 0)
            return -1;
    }

    return out_flush(t);
}
