#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pftrace.h"

/* Registers the same track layout in either mode. */
struct tracks {
    uint64_t proc;
    uint64_t main_thread;
    uint64_t worker_thread;
    uint64_t queue_depth;
};

static struct tracks setup_tracks(struct pf_trace *t)
{
    struct tracks tr;
    tr.proc = pf_track_process(t, 1234, "demo_app");
    tr.main_thread = pf_track_thread(t, tr.proc, 1234, 1, "main");
    tr.worker_thread = pf_track_thread(t, tr.proc, 1234, 2, "worker");
    tr.queue_depth = pf_track_counter(t, tr.proc, "queue_depth", PF_UNIT_COUNT);
    return tr;
}

static void do_work(struct pf_trace *t, const struct tracks *tr, int rounds)
{
    for (int i = 0; i < rounds; i++) {
        pf_slice_begin(t, tr->main_thread, "compute", "app");
        pf_counter_set_int(t, tr->queue_depth, 10 - i % 4 * 3);
        usleep(500);

        pf_slice_begin(t, tr->main_thread, "parse", "app");
        usleep(500);
        pf_slice_end(t, tr->main_thread); /* parse */

        pf_slice_begin(t, tr->main_thread, "render", "app");
        usleep(500);
        pf_slice_end(t, tr->main_thread); /* render */

        pf_slice_end(t, tr->main_thread); /* compute */
    }

    for (int i = 0; i < rounds; i++) {
        pf_slice_begin(t, tr->worker_thread, "work_item", "worker");
        pf_counter_set_double(t, tr->queue_depth, 4.5 + i);
        if (i % 2 == 0)
            pf_instant_event(t, tr->worker_thread, "cache_miss", "worker");
        usleep(500);
        pf_slice_end(t, tr->worker_thread);
    }
}

static void report(const char *label, struct pf_trace *t)
{
    struct pf_stats s;
    pf_trace_stats(t, &s);
    printf("  %s: %llu events, %llu bytes written, %llu dropped, "
           "%llu truncated, %llu resyncs\n",
           label, (unsigned long long)s.events,
           (unsigned long long)s.bytes_written, (unsigned long long)s.dropped,
           (unsigned long long)s.truncated_names,
           (unsigned long long)s.resyncs);
}

/* Snapshot mode: events accumulate in RAM and are overwritten as the ring
 * wraps. Nothing reaches disk until the program decides something went
 * wrong, at which point the buffer holds the window leading up to it. */
static int run_snapshot(void)
{
    const char *path = "demo_snapshot.pftrace";
    unlink(path); /* O_APPEND would otherwise extend a previous run */

    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = path;
    cfg.mode = PF_MODE_SNAPSHOT;
    cfg.ring_bytes = 256 * 1024;

    struct pf_trace *t = pf_trace_open(&cfg);
    if (!t) {
        perror("pf_trace_open");
        return 1;
    }

    struct tracks tr = setup_tracks(t);

    /* Plenty of work, most of it overwritten -- exactly what this mode is
     * for. Only the tail end survives in the ring. */
    do_work(t, &tr, 40);

    /* Pretend the program noticed a stall here. */
    pf_instant_event(t, tr.main_thread, "perf_drop_detected", "app");
    if (pf_trace_dump(t) != 0) {
        perror("pf_trace_dump");
        pf_trace_close(t);
        return 1;
    }

    report("snapshot", t);
    pf_trace_close(t);
    printf("  wrote %s\n", path);
    return 0;
}

/* Continuous mode: a background thread drains the ring past the watermark,
 * so the file accumulates everything rather than a window. */
static int run_continuous(void)
{
    const char *path = "demo_continuous.pftrace";
    unlink(path);

    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = path;
    cfg.mode = PF_MODE_CONTINUOUS;
    cfg.ring_bytes = 128 * 1024;
    cfg.watermark_pct = 75;
    cfg.drain_interval_ms = 20;

    struct pf_trace *t = pf_trace_open(&cfg);
    if (!t) {
        perror("pf_trace_open");
        return 1;
    }

    struct tracks tr = setup_tracks(t);

    /* Far more events than the ring holds, so this only comes out
     * gapless because the drain thread keeps up. */
    do_work(t, &tr, 60);

    if (pf_trace_flush(t) != 0) {
        perror("pf_trace_flush");
        pf_trace_close(t);
        return 1;
    }

    struct pf_stats s;
    pf_trace_stats(t, &s);
    report("continuous", t);
    if (s.dropped)
        printf("  NOTE: %llu events dropped -- ring or interval too small\n",
               (unsigned long long)s.dropped);

    pf_trace_close(t);
    printf("  wrote %s\n", path);
    return 0;
}

int main(int argc, char **argv)
{
    bool want_snapshot = true, want_continuous = true;
    if (argc > 1) {
        want_snapshot = strcmp(argv[1], "snapshot") == 0;
        want_continuous = strcmp(argv[1], "continuous") == 0;
        if (!want_snapshot && !want_continuous) {
            fprintf(stderr, "usage: %s [snapshot|continuous]\n", argv[0]);
            return 2;
        }
    }

    if (want_snapshot) {
        puts("snapshot mode (dump on demand):");
        if (run_snapshot() != 0)
            return 1;
    }
    if (want_continuous) {
        puts("continuous mode (drain past watermark):");
        if (run_continuous() != 0)
            return 1;
    }
    return 0;
}
