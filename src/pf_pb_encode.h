#ifndef PF_PB_ENCODE_H
#define PF_PB_ENCODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

struct pf_arg;
struct pf_trace;
struct pf_writer;

/* Producer-side protobuf encoding. Each event is encoded into its writer's
 * chunk as one Trace.packet, so the drain copies bytes and does not encode.
 * Field numbers are from the Perfetto schema. */

/* Trace and TracePacket */
#define FN_TRACE_PACKET 1
#define FN_PACKET_CLOCK_SNAPSHOT 6
#define FN_PACKET_TIMESTAMP 8
#define FN_PACKET_TRUSTED_SEQ_ID 10
#define FN_PACKET_TRACK_EVENT 11
#define FN_PACKET_INTERNED_DATA 12
#define FN_PACKET_SEQUENCE_FLAGS 13
#define FN_PACKET_TIMESTAMP_CLOCK_ID 58
#define FN_PACKET_DEFAULTS 59

/* ClockSnapshot and its Clock */
#define FN_CLOCK_SNAPSHOT_CLOCKS 1
#define FN_CLOCK_SNAPSHOT_PRIMARY 2
#define FN_CLOCK_ID 1
#define FN_CLOCK_TIMESTAMP 2
#define FN_CLOCK_IS_INCREMENTAL 3

/* TracePacketDefaults and TrackEventDefaults */
#define FN_DEFAULTS_TIMESTAMP_CLOCK_ID 58
#define FN_DEFAULTS_TRACK_EVENT 11
#define FN_TRACK_EVENT_DEFAULTS_TRACK_UUID 11

/* The clock event timestamps are on: sequence scoped (ids 64 to 127 are), and
 * incremental, so each timestamp is a delta from the packet before it. Each
 * chunk's header ties it to the trace clock. */
#define PF_INCREMENTAL_CLOCK_ID 64

/* TrackEvent */
#define FN_EVENT_TYPE 9
#define FN_EVENT_NAME_IID 10
#define FN_EVENT_TRACK_UUID 11
#define FN_EVENT_NAME_STR 23
#define FN_EVENT_COUNTER_VALUE 30
#define FN_EVENT_DOUBLE_COUNTER_VALUE 44
#define FN_EVENT_DEBUG_ANNOTATIONS 4

/* DebugAnnotation, from debug_annotation.proto (v58.3). Its layout is not a
 * stable Perfetto API, so readers must not depend on it. */
#define FN_ANN_UINT 3
#define FN_ANN_INT 4
#define FN_ANN_DOUBLE 5
#define FN_ANN_NAME_IID 1
#define FN_ANN_NAME 10

/* InternedData / EventName, and the same shape for DebugAnnotationName */
#define FN_INTERNED_EVENT_NAMES 2
#define FN_INTERNED_DEBUG_ANNOTATION_NAMES 3
#define FN_IID 1
#define FN_NAME_STR 2

#define SEQ_INCREMENTAL_STATE_CLEARED 1u
#define SEQ_NEEDS_INCREMENTAL_STATE 2u

/* BuiltinClock ids from ClockSnapshot; the trace clock is always one of
 * these, see pf_trace_open(). */
#define BUILTIN_CLOCK_REALTIME 1
#define BUILTIN_CLOCK_MONOTONIC 3
#define BUILTIN_CLOCK_BOOTTIME 6

static inline uint32_t builtin_clock(clockid_t clk)
{
    if (clk == CLOCK_REALTIME)
        return BUILTIN_CLOCK_REALTIME;
    if (clk == CLOCK_MONOTONIC)
        return BUILTIN_CLOCK_MONOTONIC;
    return BUILTIN_CLOCK_BOOTTIME;
}

/* Encode one event of @w's thread into its chunk, one entry point per kind of
 * event, rotating first if it does not fit. A record bigger than a whole
 * chunk, or one with no free chunk to go to, is dropped and counted. The
 * caller holds nothing; the timestamp @ts is taken by the caller, before any
 * of this. */

/* An event with a static name, interned by its pointer. */
void pf_pb_named(struct pf_trace *t, struct pf_writer *w, uint64_t ts, uint32_t type,
                 uint64_t track, const char *name);

/* pf_pb_named, with @nargs typed arguments from @args, sent as debug
 * annotations and read during the call. */
void pf_pb_named_args(struct pf_trace *t, struct pf_writer *w, uint64_t ts, uint32_t type,
                      uint64_t track, const char *name, unsigned nargs, const struct pf_arg *args);

/* An event with a dynamic name, sent inline and not interned. */
void pf_pb_dyn(struct pf_trace *t, struct pf_writer *w, uint64_t ts, uint32_t type, uint64_t track,
               const char *name);

/* A slice end: no name. */
void pf_pb_end(struct pf_trace *t, struct pf_writer *w, uint64_t ts, uint64_t track);

/* A counter sample: a value, no name. */
void pf_pb_counter(struct pf_trace *t, struct pf_writer *w, uint64_t ts, uint64_t track,
                   uint64_t raw_value, bool value_is_double);

#endif /* PF_PB_ENCODE_H */
