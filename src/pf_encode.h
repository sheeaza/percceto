#ifndef PF_ENCODE_H
#define PF_ENCODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pftrace_internal.h"

/* Producer-side protobuf encoding. Each event is encoded into its writer's
 * chunk as one Trace.packet, so the drain copies bytes and does not encode.
 * Field numbers are from the Perfetto schema, as pftrace_pb.c used them. */

/* Trace and TracePacket */
#define FN_TRACE_PACKET 1
#define FN_PACKET_TIMESTAMP 8
#define FN_PACKET_TRUSTED_SEQ_ID 10
#define FN_PACKET_TRACK_EVENT 11
#define FN_PACKET_INTERNED_DATA 12
#define FN_PACKET_SEQUENCE_FLAGS 13
#define FN_PACKET_TIMESTAMP_CLOCK_ID 58

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
#define FN_ANN_NAME 10

/* InternedData / EventName */
#define FN_INTERNED_EVENT_NAMES 2
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

/* Encodes one event of @w's thread into its chunk, rotating first if it does
 * not fit. A record bigger than a whole chunk, or one with no free chunk to
 * go to, is dropped and counted. The caller holds nothing; the timestamp is
 * taken by the caller, before any of this. A @dynamic name is sent inline and
 * not interned; a static one is interned by its pointer. @nargs typed
 * arguments, from @args, are sent as debug annotations; they are read during
 * the call. */
void pf_pb_emit(struct pf_trace *t, struct pf_writer *w, uint64_t ts, uint32_t type, uint64_t track,
                const char *name, bool dynamic, bool has_value, uint64_t raw_value,
                bool value_is_double, unsigned nargs, const struct pf_arg *args);

#endif /* PF_ENCODE_H */
