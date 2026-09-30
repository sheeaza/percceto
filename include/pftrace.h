#ifndef PFTRACE_H
#define PFTRACE_H

#include <stdint.h>

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

/* Opens `path` for writing and returns a new trace context, or NULL on
 * failure to open the file (check errno). */
struct pf_trace *pf_trace_open(const char *path);

/* Flushes any buffered output to disk without closing. */
void pf_trace_flush(struct pf_trace *t);

/* Flushes, closes the file, and frees all resources associated with `t`. */
void pf_trace_close(struct pf_trace *t);

/* Registers a process track and returns its track_uuid. */
uint64_t pf_track_process(struct pf_trace *t, int32_t pid, const char *name);

/* Registers a thread track, optionally nested under a process track
 * (pass 0 for parent_uuid if there is none), and returns its track_uuid. */
uint64_t pf_track_thread(struct pf_trace *t, uint64_t parent_uuid, int32_t pid,
                         int32_t tid, const char *name);

/* Registers a counter track, optionally nested under another track (pass
 * 0 for parent_uuid), and returns its track_uuid. */
uint64_t pf_track_counter(struct pf_trace *t, uint64_t parent_uuid,
                          const char *name, enum pf_unit unit);

/* Begins a slice on `track_uuid`. `category` may be NULL. Slices on a
 * given track must be ended in stack (LIFO) order via pf_slice_end. */
void pf_slice_begin(struct pf_trace *t, uint64_t track_uuid, const char *name,
                    const char *category);

/* Ends the most recently begun, not-yet-ended slice on `track_uuid`. */
void pf_slice_end(struct pf_trace *t, uint64_t track_uuid);

/* Records a single-point-in-time marker on `track_uuid` (Perfetto's
 * TYPE_INSTANT) — no matching _end call. `category` may be NULL. */
void pf_instant_event(struct pf_trace *t, uint64_t track_uuid, const char *name,
                      const char *category);

/* Samples a counter track with an integer or floating-point value. */
void pf_counter_set_int(struct pf_trace *t, uint64_t counter_track_uuid,
                        int64_t value);
void pf_counter_set_double(struct pf_trace *t, uint64_t counter_track_uuid,
                           double value);

#ifdef __cplusplus
}
#endif

#endif /* PFTRACE_H */
