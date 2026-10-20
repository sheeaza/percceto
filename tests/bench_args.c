/* Cost of the record-size change, and of event arguments.
 *
 * Built against both the baseline tree (32-byte records, no args API) and
 * the args tree (48-byte records). Sections that need the args API are
 * compiled only when PF_MAX_ARGS is defined.
 *
 * The library reads the clock on every event. On a machine without a fast
 * clock read that cost dwarfs everything else, so this program replaces
 * clock_gettime() with a counter for the library's timestamps. The timing
 * loops use a direct syscall, so the numbers are real. The clock cost is
 * reported separately, so the two can be compared on any machine.
 *
 * Output is one "name value unit" line per measurement.
 */

#define _GNU_SOURCE
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "dump_dir.h"
#include "pftrace.h"

/* Begin/end pairs per producer section: twice this many events. */
#define N_PAIRS 1000000u

/* Stubs the library's clock. Monotonic, so the trace stays ordered; the
 * values are not wall time. */
static uint64_t g_fake_ns;
int clock_gettime(clockid_t clk, struct timespec *ts)
{
    (void)clk;
    g_fake_ns += 16;
    ts->tv_sec = (time_t)(g_fake_ns / 1000000000u);
    ts->tv_nsec = (long)(g_fake_ns % 1000000000u);
    return 0;
}

static uint64_t real_ns(void)
{
    struct timespec ts;
    syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static double clock_cost_ns(void)
{
    /* The real clock, as the library would pay for it without the stub. */
    struct timespec ts;
    enum {
        REPS = 200000
    };
    uint64_t t0 = real_ns();
    for (int i = 0; i < REPS; i++)
        syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
    return (double)(real_ns() - t0) / REPS;
}

static size_t file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (size_t)st.st_size : 0;
}

/* Begin and end N_PAIRS slices with no args, or with two args when @args is
 * set. Returns ns per event. */
static double produce(struct pf_trace *t, uint64_t trk, bool args,
                      uint64_t pairs)
{
    uint64_t t0 = real_ns();
    for (uint64_t i = 0; i < pairs; i++) {
        if (args) {
#ifdef PF_MAX_ARGS
            pf_slice_begin_args(t, trk, "job %d took %f", (int)i, 2.5);
#endif
        } else {
            pf_slice_begin(t, trk, "job");
        }
        pf_slice_end(t, trk);
    }
    return (double)(real_ns() - t0) / (2.0 * (double)pairs);
}

/* Snapshot mode, protobuf: produce, then time the dump, which is where
 * the drain encodes. Returns ns per event for production and for the dump. */
static void snapshot_section(bool args, const char *label, size_t buffer_bytes)
{
    const char *dir = "bench_args.d";
    dir_reset(dir);

    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.mode = PF_MODE_SNAPSHOT;
    cfg.format = PF_FORMAT_PROTOBUF;
    cfg.path = dir;
    cfg.buffer_bytes = buffer_bytes;

    /* Open is timed: it is where the chunk pool is allocated. */
    uint64_t o0 = real_ns();
    struct pf_trace *t = pf_trace_open(&cfg);
    double open_ms = (double)(real_ns() - o0) / 1e6;
    if (!t) {
        perror("open");
        exit(1);
    }
    uint64_t trk = pf_track_thread(t, getpid(), 1, "main");

    struct rusage ru0, ru1;
    getrusage(RUSAGE_SELF, &ru0);
    double produce_ns = produce(t, trk, args, N_PAIRS);
    getrusage(RUSAGE_SELF, &ru1);
    printf("%s.open %.1f ms\n", label, open_ms);
    printf("%s.minflt %ld faults\n", label, ru1.ru_minflt - ru0.ru_minflt);

    char leaf[512] = "";
    uint64_t t0 = real_ns();
    if (pf_trace_dump(t, "snap", leaf, sizeof leaf) != 0) {
        fprintf(stderr, "dump failed\n");
        exit(1);
    }
    double dump_ns = (double)(real_ns() - t0) / (2.0 * N_PAIRS);

    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, leaf);
    double bytes_per_event = (double)file_size(path) / (2.0 * N_PAIRS);
    pf_trace_close(t);

    printf("%s.produce %.1f ns/event\n", label, produce_ns);
    printf("%s.dump_pb %.1f ns/event\n", label, dump_ns);
    printf("%s.bytes_pb %.1f B/event\n", label, bytes_per_event);
}

int main(void)
{
    printf("clock.real_read %.1f ns/call\n", clock_cost_ns());

    snapshot_section(false, "noargs_cold", 256u << 20);
    /* A buffer that stays in cache, so the writes do not go to RAM. */
    snapshot_section(false, "noargs_cached", 256u << 10);

#ifdef PF_MAX_ARGS
    snapshot_section(true, "args2_cold", 256u << 20);
    snapshot_section(true, "args2_cached", 256u << 10);
#endif
    return 0;
}
