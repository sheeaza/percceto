#ifndef PFTRACE_H
#define PFTRACE_H

#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define PF_API __attribute__((visibility("default")))
#else
#define PF_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct pf_trace;

enum pf_unit {
    PF_UNIT_UNSPECIFIED = 0,
    PF_UNIT_TIME_NS = 1,
    PF_UNIT_COUNT = 2,
    PF_UNIT_SIZE_BYTES = 3,
};

/**
 * pf_trace_open() - Open a trace file for writing.
 * @path: Path to the trace file to create.
 *
 * Return: A new trace context, or %NULL on failure to open the file
 * (check errno).
 */
PF_API struct pf_trace *pf_trace_open(const char *path);

/**
 * pf_trace_flush() - Flush any buffered output to disk without closing.
 * @t: Trace context.
 */
PF_API void pf_trace_flush(struct pf_trace *t);

/**
 * pf_trace_close() - Flush, close the file, and free all resources.
 * @t: Trace context to close.
 */
PF_API void pf_trace_close(struct pf_trace *t);

/**
 * pf_track_process() - Register a process track.
 * @t: Trace context.
 * @pid: Process ID to record.
 * @name: Process name, or %NULL.
 *
 * Return: The new track's track_uuid.
 */
PF_API uint64_t pf_track_process(struct pf_trace *t, int32_t pid,
                                 const char *name);

/**
 * pf_track_thread() - Register a thread track.
 * @t: Trace context.
 * @parent_uuid: Parent process track's uuid, or 0 if there is none.
 * @pid: Process ID owning the thread.
 * @tid: Thread ID to record.
 * @name: Thread name, or %NULL.
 *
 * Return: The new track's track_uuid.
 */
PF_API uint64_t pf_track_thread(struct pf_trace *t, uint64_t parent_uuid,
                                int32_t pid, int32_t tid, const char *name);

/**
 * pf_track_counter() - Register a counter track.
 * @t: Trace context.
 * @parent_uuid: Parent track's uuid, or 0 if there is none.
 * @name: Counter track name, or %NULL.
 * @unit: Unit the counter's values are measured in.
 *
 * Return: The new track's track_uuid.
 */
PF_API uint64_t pf_track_counter(struct pf_trace *t, uint64_t parent_uuid,
                                 const char *name, enum pf_unit unit);

/**
 * pf_slice_begin() - Begin a slice on a track.
 * @t: Trace context.
 * @track_uuid: Track to begin the slice on.
 * @name: Slice name.
 * @category: Slice category, or %NULL.
 *
 * Slices on a given track must be ended in stack (LIFO) order via
 * pf_slice_end().
 */
PF_API void pf_slice_begin(struct pf_trace *t, uint64_t track_uuid,
                           const char *name, const char *category);

/**
 * pf_slice_end() - End the most recently begun slice on a track.
 * @t: Trace context.
 * @track_uuid: Track whose most recently begun, not-yet-ended slice
 *      should be ended.
 */
PF_API void pf_slice_end(struct pf_trace *t, uint64_t track_uuid);

/**
 * pf_instant_event() - Record a single-point-in-time marker.
 * @t: Trace context.
 * @track_uuid: Track to record the marker on.
 * @name: Marker name.
 * @category: Marker category, or %NULL.
 *
 * Emits Perfetto's TYPE_INSTANT event -- there is no matching _end call.
 */
PF_API void pf_instant_event(struct pf_trace *t, uint64_t track_uuid,
                             const char *name, const char *category);

/**
 * pf_counter_set_int() - Sample a counter track with an integer value.
 * @t: Trace context.
 * @counter_track_uuid: Counter track to sample.
 * @value: Value to record.
 */
PF_API void pf_counter_set_int(struct pf_trace *t, uint64_t counter_track_uuid,
                               int64_t value);

/**
 * pf_counter_set_double() - Sample a counter track with a floating-point
 *      value.
 * @t: Trace context.
 * @counter_track_uuid: Counter track to sample.
 * @value: Value to record.
 */
PF_API void pf_counter_set_double(struct pf_trace *t,
                                  uint64_t counter_track_uuid, double value);

#ifdef __cplusplus
}
#endif

#endif /* PFTRACE_H */
