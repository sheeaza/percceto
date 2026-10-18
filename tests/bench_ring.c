/* The two ring backends, measured through pftrace's own hot path.
 *
 * WHAT THIS IS FOR. src/pf_ring.h selects one of two rings at compile
 * time: ringbuf_mpsc or ringbuf_zcopy. Both
 * sit under the same pftrace.c, the same protobuf encoder and the same
 * drain thread, so a difference measured here is attributable to the ring
 * and to nothing above it. Results are written up in
 * BENCHMARK_ring_backends.md.
 *
 * ONE SOURCE, TWO BINARIES. The backend is a compile-time choice, so one
 * process cannot hold two of them -- `struct pf_ring` is a different type
 * in each. CMake links this file once per backend into bench_ring_mpsc and
 * bench_ring_zcopy, and each prints its own name in
 * every row. Comparing means running both and reading the tables side
 * by side, which is also why the row label is the backend rather than the
 * scenario.
 *
 * Deliberately measured through the PUBLIC API -- pf_slice_begin and
 * friends -- not through pf_ring_reserve directly. A microbenchmark of
 * reserve alone would answer a question nobody has: what matters is the
 * cost a traced program actually pays, which includes the timestamp, the
 * four payload stores and the watermark check. The ring's share of that is
 * what differs between backends, and it is a minority of it, so the
 * headline per-event figures are closer together than the rings are.
 *
 * WHAT THE TWO BACKENDS DO DIFFERENTLY, since it is what the columns are
 * for:
 *
 *   mpsc    Ticket CAS to claim, lock-free with N producers, payload
 *           written in place. Its reader POPS each record into a private
 *           buffer -- one extra copy per event that an in-place reader would
 *           not pay.
 *   zcopy   Same storage model, but producers must be serialized by a
 *           mutex this program supplies. The lock covers the claim only;
 *           the fill and the submit happen outside it. The one backend
 *           that takes a lock on the hot path, which is the cost being
 *           priced.
 *
 * TWO MEASUREMENT TRAPS this is built around.
 *
 * First, a ring that wraps mid-run charges the measured loop for whatever
 * reclaim the backend does on eviction. tests/bench.c records that this
 * once made named events look twice as expensive as they are. So the
 * producer scenarios size the ring to hold the entire run, and the drain
 * scenario sizes it to hold the fill without evicting.
 *
 * Second, throughput in overwrite mode is not a win condition: evicting is
 * cheaper than delivering, so a backend can post a higher store rate
 * purely by dropping more. Hence the separate delivered-events column and
 * the per-delivered-event CPU normalization, and hence the drop and
 * eviction columns being printed rather than summarized away.
 *
 * Harness helpers below (now_ns, thread_cpu_ns, pin_to, cmp_dbl,
 * median_index, record_ctxsw) are copied from ../ringbuf's bench files
 * rather than shared with them, which is that project's own convention: a
 * bench file there is a self-contained translation unit. They are
 * duplicated here for the same reason.
 */

#define _GNU_SOURCE
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "dump_dir.h"
#include "pftrace.h"

/* Which backend this binary was linked against. Only for labelling: the
 * code below is identical in both, since it goes through the public
 * API. Mirrors the selector pf_ring.h switches on. */
#if defined(PF_RING_BACKEND_ZCOPY)
#define BACKEND_NAME "ringbuf_zcopy+mu"
#else
#define BACKEND_NAME "ringbuf_mpsc"
#endif

/* Repetitions per cell, and the row reports ONE of them -- see
 * median_index. Five is enough to discard a scheduler hiccup without
 * making a full run tedious. */
#define REPS 5

/* Events per throughput run, split across producers so the wall-clock
 * figure stays comparable as the producer count changes. */
#define TP_EVENTS 2000000u
#define MAX_PRODUCERS 16

/* Per-event cost runs: fewer events, since they are serial and the point
 * is the per-operation figure rather than aggregate throughput. */
#define COST_EVENTS 1000000u
#define COST_WARM 10000u

/* Consumer on CPU 0, producers spread over the rest. Pinning matters:
 * a mid-run migration shows up as a throughput cliff unrelated to the
 * code. */
#define CONSUMER_CPU 0

static int g_ncpu = 1;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/**
 * thread_cpu_ns - on-CPU time of the calling thread
 *
 * Counts only time actually running, so a thread blocked on a futex
 * accrues none of it while descheduled. That is exactly the asymmetry
 * worth measuring here: zcopy's losers park on a mutex, while the
 * lock-free backends' losers burn cycles retrying. Wall-clock throughput
 * alone would hide which one happened.
 *
 * Return: CPU time consumed by this thread, in nanoseconds.
 */
static uint64_t thread_cpu_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int producer_cpu(unsigned i)
{
    if (g_ncpu < 2)
        return CONSUMER_CPU;
    return 1 + (int)(i % (unsigned)(g_ncpu - 1));
}

/**
 * pin_to - bind the calling thread to one CPU
 * @cpu: CPU to pin to.
 *
 * Warns rather than failing: an unpinned run still produces numbers, just
 * noisier ones, and saying so is more useful than refusing to measure.
 */
static void pin_to(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
        fprintf(stderr, "warning: could not pin to cpu %d\n", cpu);
}

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/**
 * median_index - index of the median element, without reordering the input
 * @v: values to rank.
 * @n: how many.
 *
 * Used to pick one whole repetition to report, so that every column of a
 * result row comes from the same run. Taking a per-column median instead
 * would mix runs and could print a CPU figure that never accompanied the
 * printed throughput.
 *
 * Return: index into @v of the median value, or 0 if @n is 0.
 */
static int median_index(const double *v, int n)
{
    double sorted[REPS];
    memcpy(sorted, v, (size_t)n * sizeof(*v));
    qsort(sorted, (size_t)n, sizeof(*sorted), cmp_dbl);

    double target = sorted[(n - 1) / 2];
    for (int i = 0; i < n; i++)
        if (v[i] == target)
            return i;
    return 0;
}

/* --- One scenario, and one result ------------------------------------- */

/*
 * Everything that varies between measured rows. Passed by pointer rather
 * than as six positional arguments, since several are counts and sizes
 * that would be easy to transpose at a call site.
 */
struct scenario {
    enum pf_mode mode;
    size_t ring_bytes;
    unsigned producers;
    unsigned events; /* total, split across producers */
    bool named; /* emit slice_begin with a name, not slice_end */
    bool paced; /* sleep periodically, so the CPU columns inform */
};

struct result {
    double events_per_sec; /* published per second, wall clock */
    double cpu_ns_per_event; /* all threads summed, per published event */
    double cpu_per_wall; /* producers+1 means every core pegged */
    double prod_cpu_ns_per_event;
    double bytes_per_event;
    double drop_pct;
    double finish_spread; /* slowest producer / fastest, 1.0 is perfect */
    double ctxsw_per_k; /* voluntary + involuntary, every producer */
    uint64_t published;
    uint64_t dropped;
    bool accounted; /* published + dropped == attempted */
};

/* Written by the owning thread only, and _Alignas(64) on the first member
 * pads sizeof to a multiple of 64 so an array of these stays
 * line-separated. Without that, two producers' counters would share a
 * cache line and the benchmark would be measuring false sharing it
 * introduced itself. */
struct worker {
    _Alignas(64) struct pf_trace *trace;
    uint64_t track;
    unsigned id;
    unsigned events;
    bool named;
    bool paced;

    uint64_t cpu_ns;
    uint64_t elapsed_ns;
    uint64_t ctxsw;
};

static void record_ctxsw(struct worker *w)
{
    struct rusage ru;
    if (getrusage(RUSAGE_THREAD, &ru) != 0) {
        w->ctxsw = 0;
        return;
    }
    w->ctxsw = (uint64_t)ru.ru_nvcsw + (uint64_t)ru.ru_nivcsw;
}

/**
 * producer_fn - emit @events events and report what it cost
 * @arg: this thread's struct worker.
 *
 * Return: always %NULL; results come back through the worker struct.
 */
static void *producer_fn(void *arg)
{
    struct worker *w = arg;
    pin_to(producer_cpu(w->id));

    uint64_t cpu0 = thread_cpu_ns();
    uint64_t t0 = now_ns();

    for (unsigned i = 0; i < w->events; i++) {
        if (w->named)
            pf_slice_begin(w->trace, w->track, "compute");
        else
            pf_slice_end(w->trace, w->track);

        /* Paced mode only: leave the drain thread room to keep up, so the
         * CPU columns measure work rather than two threads spinning flat
         * out. Without this every backend pins cpu/wall at ~2.0 and the
         * column stops distinguishing them. */
        if (w->paced && i % 200 == 199) {
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L };
            nanosleep(&ts, NULL);
        }
    }

    w->elapsed_ns = now_ns() - t0;
    w->cpu_ns = thread_cpu_ns() - cpu0;
    record_ctxsw(w);
    return NULL;
}

/**
 * run_one - one repetition of @s
 * @s: the scenario.
 *
 * Opens a fresh trace, runs the producers, closes it (which drains what is
 * left), and reports. The trace file is removed afterwards: these runs
 * write tens of megabytes and none of it is inspected.
 *
 * Return: the measured result, zeroed if the trace could not be opened.
 */
static struct result run_one(const struct scenario *s)
{
    struct result r;
    memset(&r, 0, sizeof(r));

    const char *path = "bench_ring.pftrace";
    unlink(path);

    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = ".";
    if (s->mode == PF_MODE_CONTINUOUS)
        cfg.name = path;
    cfg.mode = s->mode;
    cfg.ring_bytes = s->ring_bytes;

    struct pf_trace *t = pf_trace_open(&cfg);
    if (!t) {
        perror("pf_trace_open");
        return r;
    }

    uint64_t track = pf_track_thread(t, 1, 1, "main");

    static struct worker prod[MAX_PRODUCERS];
    pthread_t th[MAX_PRODUCERS];
    memset(prod, 0, sizeof(prod));

    /* Remainder to the first few producers rather than dropped, so the
     * total is exactly s->events however many producers there are. */
    unsigned per = s->events / s->producers;
    unsigned extra = s->events % s->producers;

    uint64_t wall0 = now_ns();
    for (unsigned i = 0; i < s->producers; i++) {
        prod[i].trace = t;
        prod[i].track = track;
        prod[i].id = i;
        prod[i].events = per + (i < extra ? 1 : 0);
        prod[i].named = s->named;
        prod[i].paced = s->paced;
        if (pthread_create(&th[i], NULL, producer_fn, &prod[i]) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            exit(1);
        }
    }
    for (unsigned i = 0; i < s->producers; i++)
        pthread_join(th[i], NULL);
    uint64_t wall = now_ns() - wall0;

    uint64_t prod_cpu = 0, ctxsw = 0, slowest = 0, fastest = UINT64_MAX;
    for (unsigned i = 0; i < s->producers; i++) {
        prod_cpu += prod[i].cpu_ns;
        ctxsw += prod[i].ctxsw;
        if (prod[i].elapsed_ns > slowest)
            slowest = prod[i].elapsed_ns;
        if (prod[i].elapsed_ns < fastest)
            fastest = prod[i].elapsed_ns;
    }

    /* Stats before close, since close drains and would change
     * bytes_written after the timed section. */
    struct pf_stats st;
    pf_trace_stats(t, &st);

    pf_trace_close(t);
    unlink(path);

    uint64_t attempted = s->events;
    double k = (double)attempted / 1000.0;

    r.events_per_sec = (double)st.events * 1e9 / (double)wall;
    r.cpu_ns_per_event = st.events ? (double)prod_cpu / (double)st.events : 0.0;
    r.cpu_per_wall = (double)prod_cpu / (double)wall;
    r.prod_cpu_ns_per_event = r.cpu_ns_per_event;
    r.bytes_per_event =
        st.events ? (double)st.bytes_written / (double)st.events : 0.0;
    r.drop_pct = 100.0 * (double)st.dropped / (double)attempted;
    r.finish_spread = fastest ? (double)slowest / (double)fastest : 0.0;
    r.ctxsw_per_k = (double)ctxsw / k;
    r.published = st.events;
    r.dropped = st.dropped;
    r.accounted = st.events + st.dropped == attempted;
    return r;
}

/**
 * bench_one - run @s five times and return the median repetition
 * @s: the scenario.
 *
 * Return: one whole repetition, selected by its throughput -- so every
 * column of the printed row describes the same run.
 */
static struct result bench_one(const struct scenario *s)
{
    struct result all[REPS];
    double rate[REPS];

    for (int i = 0; i < REPS; i++) {
        all[i] = run_one(s);
        rate[i] = all[i].events_per_sec;
    }
    return all[median_index(rate, REPS)];
}

static void print_header(void)
{
    printf("  %-18s %9s %9s %9s %8s %8s %7s %8s\n", "backend", "Mev/s",
           "cpu-ns/ev", "cpu/wall", "drop-%", "ctxsw/k", "spread", "B/ev");
}

/**
 * print_row - one result line, plus a continuation when it has news
 * @r: the result.
 *
 * The continuation line is printed only when there is something to say --
 * a drop or a failed accounting invariant -- so a clean table
 * stays one line per row and anything unusual is impossible to miss.
 */
static void print_row(const struct result *r)
{
    printf("  %-18s %9.2f %9.1f %8.2f %8.2f %8.1f %7.2f %8.1f\n", BACKEND_NAME,
           r->events_per_sec / 1e6, r->cpu_ns_per_event, r->cpu_per_wall,
           r->drop_pct, r->ctxsw_per_k, r->finish_spread, r->bytes_per_event);

    if (r->dropped || !r->accounted) {
        printf("  %-18s  published %" PRIu64 ", dropped %" PRIu64 "%s\n", "",
               r->published, r->dropped,
               r->accounted ? "" : "  !! published + dropped != attempted");
    }
}

/* --- Per-event cost, single threaded ---------------------------------- */

/**
 * bench_cost - time one event shape with no contention and no draining
 *
 * The figure tests/bench.c reports, kept comparable with it: snapshot
 * mode, a ring big enough that nothing is ever evicted inside the loop,
 * and a warmup pass so first-touch page faults are not charged per event.
 *
 * Single-threaded, but on the same claim path a multi-threaded caller
 * takes -- so this is the uncontended cost of the real thing rather than
 * a cheaper special case. It is also where zcopy's mutex shows up at its
 * most favourable: uncontended, so the lock is a pair of uncontended
 * atomics rather than a futex.
 *
 * A FRESH TRACE PER SHAPE, which is load-bearing rather than tidy. Three
 * shapes times REPS repetitions is 15 million events, and a ring sized for
 * one run would be lapped many times over by the end -- so the later
 * shapes would be charged for first-touch faults on fresh pages and for
 * whatever the backend does on eviction, and would report two or three
 * times the cost of the first. Verified by measuring the shapes in a
 * different order in a scratch program: with one shared ring the
 * last-measured shape was always the expensive one, whichever it was,
 * while with a ring per shape all three agree to within a nanosecond.
 */
static void bench_cost(void)
{
    const char *label[3] = { "slice_end", "slice_begin+name", "counter" };
    double rate[REPS];
    double best[3] = { 0, 0, 0 };

    for (int shape = 0; shape < 3; shape++) {
        for (int rep = 0; rep < REPS; rep++) {
            const char *path = "bench_ring.pftrace";
            unlink(path);

            struct pf_config cfg;
            pf_config_init(&cfg);
            cfg.path = ".";
            cfg.mode = PF_MODE_SNAPSHOT;
            /* Holds the whole measured loop plus the warmup with room to
             * spare, so no eviction happens inside the timed section. */
            cfg.ring_bytes = 256u * 1024u * 1024u;
            cfg.drain_interval_ms = 60000; /* keep the drain thread away */

            struct pf_trace *t = pf_trace_open(&cfg);
            if (!t) {
                perror("pf_trace_open");
                return;
            }
            uint64_t trk = pf_track_thread(t, 1, 1, "main");

            /* Touch the pages this repetition will use, so first-touch
             * faults are not charged per event. */
            for (unsigned i = 0; i < COST_WARM; i++)
                pf_slice_end(t, trk);

            uint64_t t0 = now_ns();
            for (unsigned i = 0; i < COST_EVENTS; i++) {
                if (shape == 0)
                    pf_slice_end(t, trk);
                else if (shape == 1)
                    pf_slice_begin(t, trk, "compute");
                else
                    pf_counter_set_int(t, trk, (int64_t)i);
            }
            rate[rep] = (double)(now_ns() - t0) / COST_EVENTS;

            pf_trace_close(t);
            unlink(path);
        }
        best[shape] = rate[median_index(rate, REPS)];
    }

    for (int shape = 0; shape < 3; shape++)
        printf("  %-18s %-20s %6.1f ns/event\n", BACKEND_NAME, label[shape],
               best[shape]);
}

/* --- Drain cost ------------------------------------------------------- */

/**
 * bench_drain - time one dump of a pre-filled ring
 *
 * The reader side, which only matters in continuous mode, where it sets
 * the rate above which events start being dropped. This is where the
 * backends' readers differ most: the ringbuf readers pop each record into a private
 * buffer first -- an extra copy per event, which is the cost being priced.
 */
static void bench_drain(void)
{
    const unsigned fill = 400000;
    double per[REPS];

    for (int rep = 0; rep < REPS; rep++) {
        const char *dir = "bench_ring.d";
        dir_reset(dir);

        struct pf_config cfg;
        pf_config_init(&cfg);
        cfg.path = dir;
        cfg.mode = PF_MODE_SNAPSHOT;
        /* Large enough to hold the fill without evicting, so the dump
         * walks every record and the figure is per delivered event. */
        cfg.ring_bytes = 64u * 1024u * 1024u;
        cfg.drain_interval_ms = 60000;

        struct pf_trace *t = pf_trace_open(&cfg);
        if (!t) {
            perror("pf_trace_open");
            return;
        }
        uint64_t trk = pf_track_thread(t, 1, 1, "main");

        for (unsigned i = 0; i < fill; i++) {
            pf_slice_begin(t, trk, "compute");
            pf_slice_end(t, trk);
        }

        uint64_t t0 = now_ns();
        if (pf_trace_dump(t, "bench_ring", NULL, 0) != 0)
            perror("pf_trace_dump");
        per[rep] = (double)(now_ns() - t0) / (fill * 2);

        pf_trace_close(t);
        dir_reset(dir);
    }

    double p = per[median_index(per, REPS)];
    printf("  %-18s %-20s %6.1f ns/event  (%.1f M events/s)\n", BACKEND_NAME,
           "encode+write", p, 1000.0 / p);
}

/* --- Scenarios -------------------------------------------------------- */

int main(void)
{
    static const unsigned prod_counts[] = { 1, 2, 4, 7 };
    static const size_t ring_sizes[] = { 64u * 1024u, 1024u * 1024u,
                                         8u * 1024u * 1024u };

    g_ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (g_ncpu < 1)
        g_ncpu = 1;
    pin_to(CONSUMER_CPU);

    printf("pftrace ring backend benchmark -- %s\n", BACKEND_NAME);
    printf("\n");
    printf("Method: %u events per throughput run, split across producers;"
           " %d reps per\n",
           TP_EVENTS, REPS);
    printf("cell, reporting the ONE repetition whose throughput was the"
           " median, so every\n");
    printf("column of a row describes the same run. Consumer pinned to cpu"
           " %d, producers\n",
           CONSUMER_CPU);
    printf("over the remaining %d. Run both binaries and compare"
           " tables.\n",
           g_ncpu - 1);
    printf("\n");
    printf("Columns: Mev/s is events PUBLISHED per second (wall clock)."
           " cpu-ns/ev is\n");
    printf("producer CPU time per published event -- it counts only on-CPU"
           " time, so a\n");
    printf("thread parked on a mutex accrues none of it while a thread"
           " retrying a\n");
    printf("lock-free claim accrues all of it. cpu/wall at"
           " producers means every\n");
    printf("producer core pegged. spread is slowest/fastest producer, 1.00"
           " is perfect.\n");

    if (g_ncpu < 8) {
        printf("\nwarning: %d cpus online; the 7-producer rows are"
               " oversubscribed and\n",
               g_ncpu);
        printf("         their numbers reflect scheduling, not the ring.\n");
    }

    printf("\n[1] Per-event producer cost, single threaded, no draining\n");
    printf("    The number a traced program pays per call, uncontended."
           " Snapshot mode\n");
    printf("    with a 512MiB ring, so nothing is evicted inside the"
           " measured loop.\n");
    printf("    This is also zcopy's most favourable case: its mutex is"
           " uncontended\n");
    printf("    here, so it costs two atomics rather than a futex.\n");
    bench_cost();

    printf("\n[2] Drain cost: encode and write one pre-filled ring\n");
    printf("    The reader side, which sets continuous mode's ceiling."
           " Where the two\n");
    printf("    ringbuf backends pay an extra copy per event, because they"
           " pop into a private buffer instead of encoding in place.\n");
    bench_drain();

    printf("\n[3] Lossless (continuous), BY PRODUCER COUNT, 8MiB ring\n");
    printf("    Fan-in with backpressure: the drain thread must keep up or"
           " events are\n");
    printf("    dropped, and the drop column is part of the result rather"
           " than a\n");
    printf("    failure. Where a lock-free claim should beat a mutex, if it"
           " ever does.\n");
    for (unsigned p = 0; p < sizeof prod_counts / sizeof *prod_counts; p++) {
        printf("  producers=%u\n", prod_counts[p]);
        print_header();
        struct scenario s = { .mode = PF_MODE_CONTINUOUS,
                              .ring_bytes = 8u * 1024u * 1024u,
                              .producers = prod_counts[p],
                              .events = TP_EVENTS };
        struct result r = bench_one(&s);
        print_row(&r);
    }

    printf("\n[4] Snapshot (overwriting), BY PRODUCER COUNT, 8MiB ring\n");
    printf("    No backpressure, so nothing is dropped for lack of space --"
           " the ring\n");
    printf("    recycles instead. Any drop here is a backend REFUSING a"
           " claim, which\n");
    printf("    the slot-indexed pair can do.\n");
    for (unsigned p = 0; p < sizeof prod_counts / sizeof *prod_counts; p++) {
        printf("  producers=%u\n", prod_counts[p]);
        print_header();
        struct scenario s = { .mode = PF_MODE_SNAPSHOT,
                              .ring_bytes = 8u * 1024u * 1024u,
                              .producers = prod_counts[p],
                              .events = TP_EVENTS };
        struct result r = bench_one(&s);
        print_row(&r);
    }

    printf("\n[5] Lossless, 4 producers, BY RING SIZE\n");
    printf("    A small ring means the claim contends with the reader as"
           " well as with\n");
    printf("    peers, and refusals get more likely. The slot-indexed"
           " backends refuse\n");
    printf("    when a peer holds an unsubmitted slot, so this is where"
           " that shows.\n");
    for (unsigned z = 0; z < sizeof ring_sizes / sizeof *ring_sizes; z++) {
        printf("  ring=%zuKiB\n", ring_sizes[z] / 1024u);
        print_header();
        struct scenario s = { .mode = PF_MODE_CONTINUOUS,
                              .ring_bytes = ring_sizes[z],
                              .producers = 4,
                              .events = TP_EVENTS };
        struct result r = bench_one(&s);
        print_row(&r);
    }

    printf("\n[6] Snapshot, 4 producers, BY RING SIZE\n");
    printf("    The same sweep with overwriting on, so the reader never"
           " exerts\n");
    printf("    backpressure and a small ring simply recycles faster.\n");
    for (unsigned z = 0; z < sizeof ring_sizes / sizeof *ring_sizes; z++) {
        printf("  ring=%zuKiB\n", ring_sizes[z] / 1024u);
        print_header();
        struct scenario s = { .mode = PF_MODE_SNAPSHOT,
                              .ring_bytes = ring_sizes[z],
                              .producers = 4,
                              .events = TP_EVENTS };
        struct result r = bench_one(&s);
        print_row(&r);
    }

    printf("\n[7] Named events, lossless, 4 producers, 8MiB ring\n");
    printf("    slice_begin with a name rather than slice_end, so the"
           " drain thread\n");
    printf("    additionally interns a string per event. Charges the"
           " reader, not the\n");
    printf("    producer, which stores only the literal's address.\n");
    print_header();
    {
        struct scenario s = { .mode = PF_MODE_CONTINUOUS,
                              .ring_bytes = 8u * 1024u * 1024u,
                              .producers = 4,
                              .events = TP_EVENTS,
                              .named = true };
        struct result r = bench_one(&s);
        print_row(&r);
    }

    printf("\n[8] Paced, lossless, 4 producers, 8MiB ring\n");
    printf("    1ms sleep every 200 events, so producers and the drain"
           " thread are not\n");
    printf("    both spinning flat out. The ONLY scenario where the CPU"
           " columns add\n");
    printf("    information: elsewhere cpu/wall pins at the producer count"
           " and\n");
    printf("    cpu-ns/ev is just the wall cost restated.\n");
    print_header();
    {
        struct scenario s = { .mode = PF_MODE_CONTINUOUS,
                              .ring_bytes = 8u * 1024u * 1024u,
                              .producers = 4,
                              .events = TP_EVENTS / 4,
                              .paced = true };
        struct result r = bench_one(&s);
        print_row(&r);
    }

    printf("\ndone -- %s\n", BACKEND_NAME);
    return 0;
}
