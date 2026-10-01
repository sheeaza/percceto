/* Throughput of the hot path versus the drain path.
 *
 * The two matter for different reasons. Producer cost is the number the
 * library exists to minimise -- it is paid on the caller's thread, inside
 * whatever was being measured. Drain cost is paid on a background thread
 * and only matters in continuous mode, where it sets the rate above which
 * events start getting dropped.
 */

#include "pftrace.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#define N 1000000

int main(void)
{
    /* Producer cost with no draining at all: snapshot mode, ring big
     * enough that the trim thread stays idle. This is the pure
     * claim+memcpy+publish path. */
    const char *path = "bench.pftrace";
    unlink(path);

    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = path;
    cfg.mode = PF_MODE_SNAPSHOT;
    cfg.ring_bytes = 64 * 1024 * 1024;
    cfg.single_producer = true;

    struct pf_trace *t = pf_trace_open(&cfg);
    if (!t) {
        perror("open");
        return 1;
    }
    uint64_t proc = pf_track_process(t, 1, "bench");
    uint64_t trk = pf_track_thread(t, proc, 1, 1, "main");

    /* Warm the mapping so first-touch page faults are not counted as
     * per-event cost. */
    for (unsigned i = 0; i < 10000; i++)
        pf_slice_end(t, trk);

    uint64_t t0 = now_ns();
    for (unsigned i = 0; i < N; i++)
        pf_slice_end(t, trk);
    uint64_t t1 = now_ns();
    printf("producer, slice_end (32B record):      %6.1f ns/event\n",
           (double)(t1 - t0) / N);

    t0 = now_ns();
    for (unsigned i = 0; i < N; i++)
        pf_slice_begin(t, trk, "compute", "app");
    t1 = now_ns();
    printf("producer, slice_begin+name+cat (48B):  %6.1f ns/event\n",
           (double)(t1 - t0) / N);

    t0 = now_ns();
    for (unsigned i = 0; i < N; i++)
        pf_counter_set_int(t, trk, i);
    t1 = now_ns();
    printf("producer, counter (40B record):        %6.1f ns/event\n",
           (double)(t1 - t0) / N);

    pf_trace_close(t);

    /* Drain cost: fill a ring without draining, then time one dump.
     * Snapshot mode trims, so use a ring large enough to hold the lot. */
    unlink(path);
    pf_config_init(&cfg);
    cfg.path = path;
    cfg.mode = PF_MODE_SNAPSHOT;
    cfg.ring_bytes = 64 * 1024 * 1024;
    cfg.single_producer = true;
    cfg.drain_interval_ms = 60000; /* keep the trim thread out of the way */

    t = pf_trace_open(&cfg);
    if (!t) {
        perror("open");
        return 1;
    }
    proc = pf_track_process(t, 1, "bench");
    trk = pf_track_thread(t, proc, 1, 1, "main");

    const unsigned fill = 400000;
    for (unsigned i = 0; i < fill; i++) {
        pf_slice_begin(t, trk, "compute", "app");
        pf_slice_end(t, trk);
    }

    t0 = now_ns();
    if (pf_trace_dump(t) != 0)
        perror("dump");
    t1 = now_ns();

    struct pf_stats s;
    pf_trace_stats(t, &s);
    double per = (double)(t1 - t0) / (fill * 2);
    printf("drain, encode+write:                   %6.1f ns/event "
           "(%.1f M events/s)\n",
           per, 1000.0 / per);
    printf("  dumped %" PRIu64 " bytes for %u events (%.1f B/event)\n",
           s.bytes_written, fill * 2, (double)s.bytes_written / (fill * 2));

    pf_trace_close(t);
    unlink(path);
    return 0;
}
