/* Cost of the chunk buffers, by chunk size and by thread count.
 *
 * This asks two questions about the chunk design:
 *
 *  - Rotation: a producer takes the pool lock once per chunk. Sweeping
 *    chunk_slots shows what that costs per event, from a chunk of 16 records
 *    up to chunks big enough that rotation is negligible.
 *  - Scaling: each producer writes into a chunk it owns, so the per-event
 *    cost should stay flat as threads are added. The commit and drain
 *    hand-off is the only shared step.
 *
 * Both run in snapshot mode with a pool small enough to wrap many times.
 * Snapshot mode never drains and never drops, so this is the producer cost
 * alone. After the first wrap every rotation steals a committed chunk under
 * the pool lock, which is the steady-state cost a producer pays. The first
 * pass over fresh pages is short against the run, so page faults barely show.
 *
 * The library reads the clock on every event. Where clock_gettime() is a real
 * syscall rather than a vDSO read, that cost dwarfs the buffer work, so this
 * program stubs the library's clock with a counter. The timing loops use a
 * direct syscall so their numbers are real. The raw clock cost is printed on
 * the first line for comparison.
 */

#define _GNU_SOURCE
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "dump_dir.h"
#include "pftrace.h"

#define DIR "bench_chunk.d"
#define MAX_THREADS 8
#define EVENTS_PER_THREAD 2000000u

/* Stubs the library's clock. Monotonic per thread, so each thread's records
 * stay ordered; the values are not wall time. The counter is thread-local so
 * the stub adds no shared cache line for the producers to contend on. */
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

static double clock_cost_ns(void)
{
    enum {
        REPS = 200000
    };
    struct timespec ts;
    uint64_t t0 = now_ns();
    for (int i = 0; i < REPS; i++)
        syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
    return (double)(now_ns() - t0) / REPS;
}

struct worker {
    struct pf_trace *t;
    uint64_t trk;
    uint64_t pairs;
    uint64_t t0, t1;
};

static pthread_barrier_t go;

static void *emit_loop(void *arg)
{
    struct worker *w = arg;
    pthread_barrier_wait(&go);
    w->t0 = now_ns();
    for (uint64_t i = 0; i < w->pairs; i++) {
        pf_slice_begin(w->t, w->trk, "compute");
        pf_slice_end(w->t, w->trk);
    }
    w->t1 = now_ns();
    return NULL;
}

/* Runs @threads producers, each emitting @pairs begin/end pairs into a trace
 * with @slots records per chunk, and prints one line. */
static void run(unsigned threads, size_t slots, uint64_t pairs)
{
    dir_reset(DIR);

    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = DIR;
    cfg.mode = PF_MODE_SNAPSHOT;
    cfg.buffer_bytes = 8u << 20;
    cfg.chunk_slots = slots;
    cfg.max_threads = threads;
    cfg.drain_interval_ms = 60000; /* keep the trim thread out of the way */

    struct pf_trace *t = pf_trace_open(&cfg);
    if (!t) {
        perror("open");
        exit(1);
    }

    struct worker w[MAX_THREADS];
    pthread_t th[MAX_THREADS];
    pthread_barrier_init(&go, NULL, threads + 1);
    for (unsigned i = 0; i < threads; i++) {
        w[i].t = t;
        w[i].trk = pf_track_thread(t, 1, (int32_t)i + 1, NULL);
        w[i].pairs = pairs;
        pthread_create(&th[i], NULL, emit_loop, &w[i]);
    }
    pthread_barrier_wait(&go);
    for (unsigned i = 0; i < threads; i++)
        pthread_join(th[i], NULL);

    struct pf_stats s;
    pf_trace_stats(t, &s);
    pf_trace_close(t);

    uint64_t first = w[0].t0, last = w[0].t1;
    double per_thread_ns = 0;
    for (unsigned i = 0; i < threads; i++) {
        if (w[i].t0 < first)
            first = w[i].t0;
        if (w[i].t1 > last)
            last = w[i].t1;
        per_thread_ns += (double)(w[i].t1 - w[i].t0) / (2.0 * pairs);
    }
    per_thread_ns /= threads;

    uint64_t emitted = (uint64_t)threads * pairs * 2;
    double wall_s = (double)(last - first) / 1e9;
    printf("threads %u, chunk_slots %5zu: %6.1f ns/event per thread, "
           "%6.2f M events/s total, %" PRIu64 " events counted\n",
           threads, slots, per_thread_ns, (double)emitted / wall_s / 1e6, s.events);
    dir_reset(DIR);
}

int main(void)
{
    printf("cores online: %ld, raw clock_gettime: %.1f ns (not charged to the "
           "library below)\n",
           sysconf(_SC_NPROCESSORS_ONLN), clock_cost_ns());

    printf("\nRotation sweep, 1 thread, %u events:\n", EVENTS_PER_THREAD * 2);
    static const size_t slots[] = { 16, 64, 256, 1024, 4096 };
    for (unsigned i = 0; i < sizeof(slots) / sizeof(slots[0]); i++)
        run(1, slots[i], EVENTS_PER_THREAD);

    printf("\nScaling, chunk_slots 256, %u events per thread:\n", EVENTS_PER_THREAD * 2);
    for (unsigned n = 1; n <= MAX_THREADS; n *= 2)
        run(n, 256, EVENTS_PER_THREAD);

    pthread_barrier_destroy(&go);
    return 0;
}
