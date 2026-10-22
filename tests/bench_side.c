/* Per-event cost on each side of the chunk buffers: the producer that emits
 * records, and the consumer that drains them.
 *
 * bench_chunk measures the producer alone, in snapshot mode, where nothing is
 * drained. bench.c measures one snapshot dump. This one runs continuous mode,
 * so the drain thread is live while the producers emit, and reports both
 * sides of the same run:
 *
 *  - Producer: ns per event on each emitting thread, from its first emit to
 *    its last. This is what the caller pays.
 *  - Consumer: ns per event the drain still has to do after the producers
 *    stop. Closing the trace waits for the drain to write every committed
 *    chunk, so the time from the last producer finishing to pf_trace_close
 *    returning, divided by the events, is the drain's per-event backlog cost.
 *
 * The consumer figure is a tail, not the drain's total work: the drain runs
 * during the producer phase too, and that part is hidden inside the producer
 * time. Drops are reported, since a drain that cannot keep up shows up there
 * first.
 *
 * The library reads the clock on every event, so this program stubs it with a
 * counter, as bench_chunk does. The timing loops use a direct syscall.
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

#define DIR "bench_side.d"
#define MAX_THREADS 12
#define EVENTS_PER_THREAD 2000000u

/* Stubs the library's clock. Thread-local, as in bench_chunk, so the stub adds
 * no shared cache line for the producers to contend on. */
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

/* Runs @threads producers in continuous mode with @slots records per chunk,
 * then prints one line with both sides. */
static void run(unsigned threads, size_t slots, uint64_t pairs)
{
    dir_reset(DIR);

    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = DIR;
    cfg.name = "bench_side.pftrace";
    cfg.mode = PF_MODE_CONTINUOUS;
    cfg.buffer_bytes = 2048u << 20;
    cfg.chunk_slots = slots;
    cfg.max_threads = threads;

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

    /* The producers are done; time how long the drain takes to finish. Stats
     * are read before close, since close frees the trace. */
    uint64_t producers_done = now_ns();
    struct pf_stats s;
    pf_trace_stats(t, &s);
    pf_trace_close(t);
    uint64_t closed = now_ns();

    uint64_t emitted = (uint64_t)threads * pairs * 2;
    double producer_ns = 0;
    for (unsigned i = 0; i < threads; i++)
        producer_ns += (double)(w[i].t1 - w[i].t0) / (2.0 * pairs);
    producer_ns /= threads;
    double consumer_ns = (double)(closed - producers_done) / emitted;

    printf("threads %u, chunk_slots %5zu: producer %6.1f ns/event, "
           "consumer tail %6.1f ns/event, %" PRIu64 " events, %" PRIu64 " dropped\n",
           threads, slots, producer_ns, consumer_ns, emitted, s.dropped);
    dir_reset(DIR);
}

int main(void)
{
    printf("cores online: %ld\n", sysconf(_SC_NPROCESSORS_ONLN));

    printf("\nContinuous mode, %u events per thread:\n", EVENTS_PER_THREAD * 2);
    static const unsigned threads[] = { 1, 4, 8 };
    static const size_t slots[] = { 4096 };
    for (unsigned s = 0; s < sizeof(slots) / sizeof(slots[0]); s++)
        for (unsigned i = 0; i < sizeof(threads) / sizeof(threads[0]); i++)
            run(threads[i], slots[s], EVENTS_PER_THREAD);

    pthread_barrier_destroy(&go);
    return 0;
}
