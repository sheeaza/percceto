/* Per-event cost on each side of the chunk buffers: the producer that emits
 * records, and the consumer that drains them.
 *
 * This program runs continuous mode, so the drain thread is live while the
 * producers emit, and reports both sides of the same run:
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
 * Two workloads run at each thread count:
 *
 *  - simple: one name, begin and end, nothing else. The floor for the cost of
 *    a record.
 *  - realistic: a mix closer to a real program. Each iteration begins a slice
 *    from a set of 16 reused names, ends it, and every 8th, 16th and 32nd
 *    iteration adds an instant with an argument, a plain instant, and a
 *    counter sample. About 1.4 records per iteration, so events per thread are
 *    counted from the emits, not assumed.
 *
 * The library reads the clock on every event, so this program stubs it with a
 * counter. The timing loops use a direct syscall.
 */

#define _GNU_SOURCE
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "dump_dir.h"
#include "pftrace.h"

#define DIR "bench_side.d"
#define MAX_THREADS 256
#define MAX_CPUS 256
#define EVENTS_PER_THREAD 2000000u

/* Stubs the library's clock. Thread-local, so the stub adds no shared cache
 * line for the producers to contend on. */
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

/* Names for the realistic workload. String literals are static, so the
 * library may keep the pointers, as pf_slice_begin_impl requires. */
static const char *const kNames[16] = {
    "parse", "decode", "lookup", "encode", "validate", "fetch", "merge", "sort",
    "hash",  "copy",   "filter", "reduce", "index",    "flush", "wait",  "notify",
};

enum workload {
    SIMPLE,
    REALISTIC,
    FORMATTED,
    MANUAL,
    ARGS
};

static const char *const kLabels[] = { "simple", "realistic", "formatted", "manual", "args" };
static int want_raw; /* set by "raw" on the command line */
static int want_pin; /* set by "pin" on the command line */

/* One line per worker: each thread adds to @events on every iteration, so
 * workers packed together would share lines and slow each other down. */
struct worker {
    _Alignas(64) struct pf_trace *t;
    uint64_t trk;
    uint64_t pairs;
    enum workload load;
    uint64_t events;
    uint64_t t0, t1;
};

static pthread_barrier_t go;

/* Hybrid CPUs, from sysfs. Producers are pinned to P cores first, then E
 * cores, so each run can report how many ran on each core type. On a machine
 * without the cpu_core and cpu_atom lists, nothing is pinned. */
static unsigned g_pcpu[MAX_CPUS], g_ecpu[MAX_CPUS];
static unsigned g_np, g_ne;

static unsigned read_cpu_list(const char *path, unsigned *out, unsigned max)
{
    char buf[256];
    unsigned n = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    if (fgets(buf, sizeof(buf), f)) {
        char *p = buf;
        while (*p >= '0' && *p <= '9') {
            unsigned lo = (unsigned)strtoul(p, &p, 10);
            unsigned hi = lo;
            if (*p == '-')
                hi = (unsigned)strtoul(p + 1, &p, 10);
            for (unsigned c = lo; c <= hi && n < max; c++)
                out[n++] = c;
            if (*p == ',')
                p++;
        }
    }
    fclose(f);
    return n;
}

/* Pins producer @i to its CPU when "pin" is given, and counts it as P when it
 * lands on one. Unpinned, the scheduler places producers. */
static void pin_producer(pthread_t th, unsigned i, unsigned *on_p)
{
    unsigned all = g_np + g_ne;
    cpu_set_t set;
    unsigned cpu;
    if (!want_pin || all == 0)
        return;
    if (i % all < g_np) {
        cpu = g_pcpu[i % all];
        (*on_p)++;
    } else {
        cpu = g_ecpu[i % all - g_np];
    }
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    pthread_setaffinity_np(th, sizeof(set), &set);
}

static void *emit_loop(void *arg)
{
    struct worker *w = arg;
    pthread_barrier_wait(&go);
    w->t0 = now_ns();
    for (uint64_t i = 0; i < w->pairs; i++) {
        if (w->load == SIMPLE) {
            pf_slice_begin(w->t, w->trk, "compute");
            pf_slice_end(w->t, w->trk);
            w->events += 2;
            continue;
        }
        if (w->load == FORMATTED) {
            pf_slice_begin(w->t, w->trk, "req-%u", (unsigned)(i & 255u));
            pf_slice_end(w->t, w->trk);
            w->events += 2;
            continue;
        }
        if (w->load == MANUAL) {
            char buf[32];
            snprintf(buf, sizeof(buf), "req-%u", (unsigned)(i & 255u));
            pf_slice_begin_dyn_impl(w->t, w->trk, buf);
            pf_slice_end(w->t, w->trk);
            w->events += 2;
            continue;
        }
        if (w->load == ARGS) {
            pf_slice_begin_args(w->t, w->trk, "compute", PF_KV("i", i), PF_KV("ratio", 0.5));
            pf_slice_end(w->t, w->trk);
            w->events += 2;
            continue;
        }
        pf_slice_begin_impl(w->t, w->trk, kNames[i & 15]);
        w->events += 1;
        if ((i & 7) == 0) {
            char job[32];
            snprintf(job, sizeof(job), "job %d took %f", (int)(i & 255), 2.5);
            pf_instant_event(w->t, w->trk, job);
            w->events += 1;
        }
        if ((i & 15) == 0) {
            pf_instant_event(w->t, w->trk, "tick");
            w->events += 1;
        }
        if ((i & 31) == 0) {
            pf_counter_set_int(w->t, w->trk, (int64_t)i);
            w->events += 1;
        }
        pf_slice_end(w->t, w->trk);
        w->events += 1;
    }
    w->t1 = now_ns();
    return NULL;
}

/* Runs @threads producers in continuous mode with @slots records per chunk,
 * then prints one line with both sides. */
static void run(unsigned threads, size_t slots, uint64_t pairs, enum workload load)
{
    dir_reset(DIR);

    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = DIR;
    cfg.name = want_raw ? "bench_side.pfraw" : "bench_side.pftrace";
    cfg.format = want_raw ? PF_FORMAT_RAW : PF_FORMAT_PROTOBUF;
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
    unsigned on_p = 0;
    for (unsigned i = 0; i < threads; i++) {
        w[i].t = t;
        w[i].trk = pf_track_thread(t, 1, (int32_t)i + 1, NULL);
        w[i].pairs = pairs;
        w[i].load = load;
        w[i].events = 0;
        pthread_create(&th[i], NULL, emit_loop, &w[i]);
        pin_producer(th[i], i, &on_p);
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

    uint64_t emitted = 0;
    double producer_ns = 0;
    for (unsigned i = 0; i < threads; i++) {
        emitted += w[i].events;
        producer_ns += (double)(w[i].t1 - w[i].t0) / (double)w[i].events;
    }
    producer_ns /= threads;
    double consumer_ns = (double)(closed - producers_done) / emitted;

    char place[32];
    if (want_pin)
        snprintf(place, sizeof(place), "P %2u, E %2u", on_p, threads - on_p);
    else
        snprintf(place, sizeof(place), "unpinned");
    printf("%-9s threads %2u (%s), chunk_slots %5zu: producer %6.1f ns/event, "
           "consumer tail %6.1f ns/event, %" PRIu64 " events, %" PRIu64 " dropped\n",
           kLabels[load], threads, place, slots, producer_ns, consumer_ns, emitted, s.dropped);
    dir_reset(DIR);
}

/* True for the first hardware thread of its core, the one listed first in its
 * sibling list. A CPU with no topology file counts as primary. */
static unsigned is_primary(unsigned cpu)
{
    char path[96];
    unsigned sib[MAX_CPUS];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/topology/thread_siblings_list",
             cpu);
    unsigned n = read_cpu_list(path, sib, MAX_CPUS);
    return n == 0 || sib[0] == cpu;
}

/* Puts the first hardware thread of each core in front, so producers spread
 * over physical cores before any two share one. */
static void order_primary_first(unsigned *cpus, unsigned n)
{
    unsigned tmp[MAX_CPUS];
    unsigned m = 0;
    for (unsigned i = 0; i < n; i++)
        if (is_primary(cpus[i]))
            tmp[m++] = cpus[i];
    for (unsigned i = 0; i < n; i++)
        if (!is_primary(cpus[i]))
            tmp[m++] = cpus[i];
    memcpy(cpus, tmp, n * sizeof(*cpus));
}

int main(int argc, char **argv)
{
    want_raw = argc > 1 && strcmp(argv[1], "raw") == 0;
    for (int a = 1; a < argc; a++)
        if (strcmp(argv[a], "pin") == 0)
            want_pin = 1;
    g_np = read_cpu_list("/sys/devices/cpu_core/cpus", g_pcpu, MAX_CPUS);
    order_primary_first(g_pcpu, g_np);
    g_ne = read_cpu_list("/sys/devices/cpu_atom/cpus", g_ecpu, MAX_CPUS);
    order_primary_first(g_ecpu, g_ne);
    printf("cores online: %ld, P cores: %u, E cores: %u, format: %s\n",
           sysconf(_SC_NPROCESSORS_ONLN), g_np, g_ne, want_raw ? "raw" : "protobuf");

    printf("\nContinuous mode, %u iterations per thread:\n", EVENTS_PER_THREAD);
    /* 1 thread, then steps of 2 up to 16 CPUs or steps of 4 above, up to twice
     * the online CPU count. */
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    unsigned step = ncpu <= 16 ? 2 : 4;
    unsigned counts[MAX_THREADS];
    unsigned nc = 0;
    counts[nc++] = 1;
    for (unsigned t = step; t <= 2u * (unsigned)ncpu && nc < MAX_THREADS; t += step)
        counts[nc++] = t;
    static const size_t slots[] = { 4096 };
    static const enum workload loads[] = { SIMPLE, REALISTIC, FORMATTED, MANUAL, ARGS };
    for (unsigned s = 0; s < sizeof(slots) / sizeof(slots[0]); s++)
        for (unsigned l = 0; l < sizeof(loads) / sizeof(loads[0]); l++)
            for (unsigned i = 0; i < nc; i++)
                run(counts[i], slots[s], EVENTS_PER_THREAD, loads[l]);

    pthread_barrier_destroy(&go);
    return 0;
}
