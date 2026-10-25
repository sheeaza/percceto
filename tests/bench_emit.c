/* Producer cost per event kind, for each output format.
 *
 * bench_chunk times one shape, a static begin and an end. The emit path
 * branches on the kind of event: a counter has a value and no name, a dynamic
 * name is copied, arguments add a section or annotations. This one times each
 * kind on its own, so a change to one branch shows on its line rather than in
 * an average.
 *
 * One thread, snapshot mode, with a pool small enough to wrap, so rotation is
 * in the steady state and nothing drains. The library's clock is stubbed with
 * a counter, as bench_chunk does; the timing uses a direct syscall. Each line
 * is the best of several runs, which is steadier than the mean on a busy box.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "dump_dir.h"
#include "pftrace.h"

#define DIR "bench_emit.d"
#define EVENTS 4000000u
#define RUNS 5

static __thread uint64_t g_fake_ns;
int clock_gettime(clockid_t clk, struct timespec *ts)
{
    (void)clk;
    g_fake_ns += 16;
    ts->tv_sec = (time_t)(g_fake_ns / 1000000000u);
    ts->tv_nsec = (long)(g_fake_ns % 1000000000u);
    return 0;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

enum kind {
    K_SLICE_BEGIN,
    K_SLICE_END,
    K_SLICE_DYN,
    K_INSTANT,
    K_INSTANT_ARGS,
    K_SLICE_ARGS3,
    K_COUNTER_INT,
    K_COUNTER_DOUBLE,
    K_COUNT
};

static const char *const kKindNames[K_COUNT] = {
    "slice_begin",    "slice_end",           "slice_begin_dyn", "instant",
    "instant, 1 arg", "slice_begin, 3 args", "counter int",     "counter double",
};

static void emit(struct pf_trace *t, uint64_t trk, enum kind k, uint32_t n)
{
    char buf[32];
    switch (k) {
    case K_SLICE_BEGIN:
        for (uint32_t i = 0; i < n; i++)
            pf_slice_begin(t, trk, "compute");
        break;
    case K_SLICE_END:
        for (uint32_t i = 0; i < n; i++)
            pf_slice_end(t, trk);
        break;
    case K_SLICE_DYN:
        memcpy(buf, "frame-0000000", 14);
        for (uint32_t i = 0; i < n; i++) {
            buf[12] = (char)('0' + i % 10);
            pf_slice_begin_dyn_impl(t, trk, buf);
        }
        break;
    case K_INSTANT:
        for (uint32_t i = 0; i < n; i++)
            pf_instant_event(t, trk, "tick");
        break;
    case K_INSTANT_ARGS:
        for (uint32_t i = 0; i < n; i++)
            pf_instant_event_args(t, trk, "tick", PF_KV("i", i));
        break;
    case K_SLICE_ARGS3:
        for (uint32_t i = 0; i < n; i++)
            pf_slice_begin_args(t, trk, "compute", PF_KV("i", i), PF_KV("ratio", 0.5),
                                PF_KV("n", -1));
        break;
    case K_COUNTER_INT:
        for (uint32_t i = 0; i < n; i++)
            pf_counter_set_int(t, trk, (int64_t)i);
        break;
    case K_COUNTER_DOUBLE:
        for (uint32_t i = 0; i < n; i++)
            pf_counter_set_double(t, trk, (double)i * 0.5);
        break;
    default:
        break;
    }
}

static double run(enum pf_format fmt, enum kind k)
{
    double best = 1e30;
    for (int r = 0; r < RUNS; r++) {
        dir_reset(DIR);
        struct pf_config cfg;
        pf_config_init(&cfg);
        cfg.path = DIR;
        cfg.mode = PF_MODE_SNAPSHOT;
        cfg.format = fmt;
        cfg.buffer_bytes = 8u << 20;
        cfg.drain_interval_ms = 60000; /* keep the trim thread out of the way */
        struct pf_trace *t = pf_trace_open(&cfg);
        if (!t) {
            perror("open");
            exit(1);
        }
        uint64_t trk = pf_track_thread(t, 1, 1, "main");
        emit(t, trk, k, 100000); /* warm the pool's pages */
        uint64_t t0 = now_ns();
        emit(t, trk, k, EVENTS);
        double ns = (double)(now_ns() - t0) / EVENTS;
        if (ns < best)
            best = ns;
        pf_trace_close(t);
    }
    dir_reset(DIR);
    return best;
}

int main(void)
{
    printf("%-22s %10s %10s\n", "ns/event", "protobuf", "raw");
    for (int k = 0; k < K_COUNT; k++) {
        double pb = run(PF_FORMAT_PROTOBUF, (enum kind)k);
        double raw = run(PF_FORMAT_RAW, (enum kind)k);
        printf("%-22s %10.2f %10.2f\n", kKindNames[k], pb, raw);
    }
    return 0;
}
