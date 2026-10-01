#ifndef PFTRACE_H
#define PFTRACE_H

#include <stdbool.h>
#include <stddef.h>
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
 * enum pf_mode - When buffered events reach the output file.
 * @PF_MODE_SNAPSHOT: Never write unless asked. The ring overwrites its
 *      oldest events indefinitely; pf_trace_dump() captures the window
 *      leading up to the call. Use this to debug a perf drop the program
 *      detects itself.
 * @PF_MODE_CONTINUOUS: Write automatically. Once the ring passes
 *      &pf_config.watermark_pct a background thread drains it, so the
 *      output file accumulates every event rather than a window. Events
 *      are dropped (and counted in &pf_stats.dropped) only if producers
 *      outrun that thread.
 */
enum pf_mode {
    PF_MODE_SNAPSHOT = 0,
    PF_MODE_CONTINUOUS = 1,
};

/**
 * struct pf_config - Trace parameters, fixed for the life of the trace.
 * @path: Output file path. Required. Opened with O_APPEND, so an
 *      existing file is extended rather than truncated.
 * @ring_bytes: In-RAM ring buffer size. Rounded up to a power of two and
 *      a page multiple. 0 selects 8 MiB. This is the only sizeable
 *      allocation the library makes, and it happens once, at open.
 * @mode: Snapshot or continuous; see &enum pf_mode.
 * @watermark_pct: Ring fill percentage that triggers an automatic drain,
 *      1-99. 0 selects 75. Ignored unless @mode is %PF_MODE_CONTINUOUS.
 * @drain_interval_ms: Upper bound on how long a buffered event waits
 *      before the drain thread considers writing it. 0 selects 100.
 *      Ignored unless @mode is %PF_MODE_CONTINUOUS.
 * @max_tracks: Capacity of the track table. 0 selects 256. Tracks are
 *      re-emitted at the head of every dump, so they cost file space
 *      rather than ring space.
 * @single_producer: Set only if exactly one thread will ever emit
 *      events. Drops the atomic claim to a plain load/store pair. The
 *      default (false) is safe for any number of threads.
 */
struct pf_config {
    const char *path;
    size_t ring_bytes;
    enum pf_mode mode;
    unsigned watermark_pct;
    unsigned drain_interval_ms;
    unsigned max_tracks;
    bool single_producer;
};

/**
 * struct pf_stats - Cumulative counters, for checking a trace is honest.
 * @events: Events successfully published to the ring.
 * @bytes_written: Bytes handed to write() across all dumps.
 * @dropped: Events discarded because the ring was full of data the drain
 *      thread had not yet written. Only ever nonzero in
 *      %PF_MODE_CONTINUOUS, where it means the output file has a gap:
 *      either the ring or the drain interval is too small.
 * @truncated_names: Names or categories longer than 255 bytes, which are
 *      recorded truncated.
 * @resyncs: Times the drain thread lost its place in the ring and
 *      skipped to the newest data, discarding what it had not yet read.
 *      Expected to stay 0; a nonzero value in %PF_MODE_SNAPSHOT means
 *      producers lapped the reader mid-dump.
 */
struct pf_stats {
    uint64_t events;
    uint64_t bytes_written;
    uint64_t dropped;
    uint64_t truncated_names;
    uint64_t resyncs;
};

/**
 * pf_config_init() - Fill a config with defaults.
 * @cfg: Config to initialize.
 *
 * Sets every field to its default, leaving &pf_config.path %NULL for the
 * caller to set. Call this before changing individual fields so later
 * additions to the struct keep their defaults.
 */
PF_API void pf_config_init(struct pf_config *cfg);

/**
 * pf_trace_open() - Allocate the ring and open the output file.
 * @cfg: Trace parameters. Copied, so it need not outlive this call.
 *
 * Performs every allocation the trace will ever need, and in
 * %PF_MODE_CONTINUOUS starts the drain thread. After this returns, the
 * event functions neither allocate nor block on I/O.
 *
 * Return: A new trace context, or %NULL on failure, with errno set.
 * %EINVAL means @cfg or its path is %NULL, or @watermark_pct exceeds 99.
 */
PF_API struct pf_trace *pf_trace_open(const struct pf_config *cfg);

/**
 * pf_trace_dump() - Write everything currently buffered.
 * @t: Trace context.
 *
 * Writes the ring's contents to the output file, preceded by the track
 * table so the result stands alone as a trace. In %PF_MODE_SNAPSHOT this
 * is the only thing that produces output -- call it when the program
 * notices the problem worth debugging.
 *
 * Safe to call from any thread, and safe to call concurrently with the
 * drain thread.
 *
 * Return: 0 on success, or -1 with errno set from write().
 */
PF_API int pf_trace_dump(struct pf_trace *t);

/**
 * pf_trace_flush() - Dump, then force the result to disk.
 * @t: Trace context.
 *
 * pf_trace_dump() plus fdatasync(). Use before a crash is likely; a
 * plain dump is enough for the file to be readable by other processes.
 *
 * Return: 0 on success, or -1 with errno set.
 */
PF_API int pf_trace_flush(struct pf_trace *t);

/**
 * pf_trace_stats() - Read the cumulative counters.
 * @t: Trace context.
 * @out: Filled with a snapshot of the counters.
 *
 * Worth checking after a run in %PF_MODE_CONTINUOUS: a nonzero
 * &pf_stats.dropped means the trace is missing events.
 */
PF_API void pf_trace_stats(const struct pf_trace *t, struct pf_stats *out);

/**
 * pf_trace_close() - Dump what is buffered, then free everything.
 * @t: Trace context to close, or %NULL.
 *
 * Stops and joins the drain thread, writes any remaining buffered
 * events, closes the file and unmaps the ring. Not safe to call
 * concurrently with any other function on @t.
 */
PF_API void pf_trace_close(struct pf_trace *t);

/**
 * pf_track_process() - Register a process track.
 * @t: Trace context.
 * @pid: Process ID to record.
 * @name: Process name, or %NULL.
 *
 * Tracks are held in a fixed-capacity table (&pf_config.max_tracks) and
 * re-emitted at the head of every dump, so events always resolve against
 * a known track even once the ring has wrapped. Not a hot-path call.
 *
 * Return: The new track's track_uuid, or 0 if the table is full.
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
 * Return: The new track's track_uuid, or 0 if the table is full.
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
 * Return: The new track's track_uuid, or 0 if the table is full.
 */
PF_API uint64_t pf_track_counter(struct pf_trace *t, uint64_t parent_uuid,
                                 const char *name, enum pf_unit unit);

/**
 * pf_slice_begin() - Begin a slice on a track.
 * @t: Trace context.
 * @track_uuid: Track to begin the slice on.
 * @name: Slice name. Truncated at 255 bytes.
 * @category: Slice category, or %NULL. Truncated at 255 bytes.
 *
 * Copies its arguments into the ring and returns; no allocation, no
 * I/O, and no lock unless the ring is being drained.
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
 * @name: Marker name. Truncated at 255 bytes.
 * @category: Marker category, or %NULL. Truncated at 255 bytes.
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
