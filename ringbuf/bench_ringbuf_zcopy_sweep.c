/*
 * ringbuf_zcopy+mu alone, swept by producer count, past the CPU count.
 *
 * A narrower companion to bench_ringbuf_mpsc_vs_ringbuf_zcopy.c. That file
 * compares ringbuf_mpsc with ringbuf_zcopy across several scenarios and stops
 * at 7 producers. This one measures only the caller-serialized arrangement:
 *
 *     pthread_mutex_lock(&push_lock);        -- serialize the CLAIM
 *     bool got = ringbuf_zcopy_reserve(rb, &rsv);
 *     pthread_mutex_unlock(&push_lock);
 *
 *     if (got) {
 *             fill(rsv.data);                -- OUTSIDE the lock
 *             ringbuf_zcopy_submit(rb, &rsv);
 *     }
 *
 * The retry loop lives outside push_lock, and submit is called with the lock
 * released, for the reason ringbuf_zcopy.h rule 2 gives: a refused reserve is
 * usually blocked by a peer's unsubmitted slot.
 *
 * The question this file answers is how throughput behaves as producers are
 * added, past the CPU count. One producer is a control with no caller mutex,
 * since a lone producer is the SPSC case. Slot size and capacity are fixed.
 * Lossless and overwrite are both measured.
 *
 * The consumer is pinned to CPU 0 and producers cycle over CPUs 1..N-1, so
 * above N-1 producers they share cores. That is the oversubscribed regime the
 * other benchmark's scenario [7] probes at 16 producers on 7 CPUs.
 */

#define _GNU_SOURCE

#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "ringbuf_zcopy.h"

#define REPS 5
#define TP_MESSAGES 2000000u

#define SLOT_SIZE 40
#define CAPACITY (1u << 18)

/* Upper bound on producers. Sized for the widest row in the sweep. */
#define MAX_PRODUCERS 32

/* Failed reserves and pops yield after this many consecutive failures. */
#define YIELD_SPINS 64u

/* Failed pops tolerated after every producer has finished before the consumer
 * calls the stream drained. See the consumer comment. */
#define DRAIN_SPINS 10000u

/* Producer counts to sweep. The last entry exceeds the online CPU count, which
 * main() checks against at run time. */
static const unsigned prod_counts[] = { 1, 4, 8, 12, 16, 20, 24 };

#define CONSUMER_CPU 0

/* CPU topology, filled in once by main(). */
static int g_ncpu = 1;

/**
 * producer_cpu - CPU to pin producer @i to
 * @i: producer index
 *
 * Producers take the CPUs above the consumer's, cycling once they run out.
 *
 * Return: a CPU number in [1, g_ncpu), or 0 on a single-CPU host.
 */
static int producer_cpu(unsigned i)
{
    if (g_ncpu < 2)
        return CONSUMER_CPU;
    return 1 + (int)(i % (unsigned)(g_ncpu - 1));
}

/**
 * cpu_relax - hint to the core that we are in a spin-wait
 */
static void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    atomic_thread_fence(memory_order_relaxed);
#endif
}

/**
 * backoff - pause after a failed attempt, yielding once it persists
 * @spins: caller's consecutive-failure counter, updated in place
 *
 * Reset @spins to 0 on success.
 */
static void backoff(unsigned *spins)
{
    if (++*spins < YIELD_SPINS) {
        cpu_relax();
        return;
    }
    *spins = 0;
    sched_yield();
}

/**
 * now_ns - read the monotonic clock
 *
 * Return: nanoseconds since an unspecified epoch, suitable for intervals.
 */
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/**
 * thread_cpu_ns - CPU time consumed by the calling thread
 *
 * Return: nanoseconds of on-CPU time charged to this thread.
 */
static uint64_t thread_cpu_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/**
 * pin_to - bind the calling thread to one CPU
 * @cpu: CPU number to pin to
 *
 * Warns rather than failing: an unpinned run still produces numbers, just
 * noisier.
 */
static void pin_to(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
        fprintf(stderr, "warning: could not pin to cpu %d\n", cpu);
}

/**
 * cmp_dbl - qsort comparator for double
 * @a: first element
 * @b: second element
 *
 * Return: negative, zero, or positive as *@a orders before, same as, or after
 * *@b.
 */
static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;
    return (x > y) - (x < y);
}

/**
 * median_index - index of the median element, without reordering the input
 * @v: samples; left untouched
 * @n: number of samples; must be positive
 *
 * Return: index into @v of its median value.
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

/**
 * fill - write a payload into a slot
 * @dst: SLOT_SIZE writable bytes
 * @key: value the written pattern is derived from
 *
 * One word-wise pass through a volatile pointer, so the compiler cannot elide
 * it. The first word carries @key unmodified for the consumer's check.
 * SLOT_SIZE is a multiple of 8, so the word loop covers every byte.
 */
static void fill(void *dst, uint64_t key)
{
    volatile uint64_t *vp = dst;
    size_t words = SLOT_SIZE / sizeof(uint64_t);

    for (size_t i = 0; i < words; i++)
        vp[i] = key + i;
    vp[0] = key;
}

/**
 * struct queue - the ringbuf_zcopy buffer and the caller's producer mutex
 *
 * _Alignas(64) keeps push_lock off the line the producers read on every
 * claim, so the measurement does not include false sharing from this harness.
 * @locked is false for a single producer: one thread is the only reserver, so
 * the SPSC contract already holds and the caller mutex has nothing to serialize.
 * It can be forced on for one producer to price the uncontended lock.
 */
struct queue {
    struct ringbuf_zcopy *zc;
    bool locked;
    _Alignas(64) pthread_mutex_t push_lock;
};

/**
 * q_init - create the queue and its producer mutex
 * @q: queue to initialize
 * @overwrite: true for RINGBUF_DROP_OLDEST, false for RINGBUF_DROP_NEWEST
 * @producers: number of producer threads that will call q_store()
 * @locked: take the caller mutex around every reserve
 *
 * Return: true on success; false if allocation or mutex init failed.
 */
static bool q_init(struct queue *q, bool overwrite, bool locked)
{
    memset(q, 0, sizeof(*q));
    q->locked = locked;
    q->zc = ringbuf_zcopy_create(SLOT_SIZE, CAPACITY,
                                 overwrite ? RINGBUF_DROP_OLDEST :
                                             RINGBUF_DROP_NEWEST);
    if (!q->zc)
        return false;
    if (pthread_mutex_init(&q->push_lock, NULL) != 0) {
        ringbuf_zcopy_destroy(q->zc);
        q->zc = NULL;
        return false;
    }
    return true;
}

/**
 * q_destroy - release a queue from q_init()
 * @q: the queue
 */
static void q_destroy(struct queue *q)
{
    pthread_mutex_destroy(&q->push_lock);
    ringbuf_zcopy_destroy(q->zc);
}

/**
 * q_store - reserve under the caller's mutex, then fill and submit
 * @q: the queue
 * @key: value the payload pattern is derived from
 *
 * The lock covers the claim only. Fill and submit run with it released, which
 * is the arrangement under test. With one producer there is no lock at all.
 *
 * Return: true if stored; false if refused. The caller retries with no lock
 * held.
 */
static bool q_store(struct queue *q, uint64_t key)
{
    struct ringbuf_rsv rsv;

    if (q->locked)
        pthread_mutex_lock(&q->push_lock);
    bool got = ringbuf_zcopy_reserve(q->zc, &rsv);
    if (q->locked)
        pthread_mutex_unlock(&q->push_lock);

    if (!got)
        return false;

    fill(rsv.data, key);
    ringbuf_zcopy_submit(q->zc, &rsv);
    return true;
}

/* ---- Worker threads ---- */

/*
 * _Alignas(64) on the first member keeps each worker on its own cache line,
 * since every thread writes its own instance on every run.
 */
struct worker {
    _Alignas(64) struct queue *q;
    unsigned id;
    uint64_t count; /* producer: messages to send */
    atomic_uint *producers_left;

    /* Written by the owning thread only. */
    uint64_t retries;
    uint64_t received;
    uint64_t cpu_ns;
    uint64_t elapsed_ns;
    uint64_t vol_ctxsw;
    uint64_t invol_ctxsw;
};

/**
 * record_ctxsw - sample this thread's context-switch counters
 * @w: worker to store the counts into
 */
static void record_ctxsw(struct worker *w)
{
    struct rusage ru;
    if (getrusage(RUSAGE_THREAD, &ru) != 0) {
        w->vol_ctxsw = 0;
        w->invol_ctxsw = 0;
        return;
    }
    w->vol_ctxsw = (uint64_t)ru.ru_nvcsw;
    w->invol_ctxsw = (uint64_t)ru.ru_nivcsw;
}

/**
 * producer_thread - pin, then store count messages
 * @arg: struct worker
 *
 * Decrements producers_left only after its last submit, so the consumer can
 * tell a head-of-line wait from a drained stream.
 *
 * Return: NULL always; results are reported through @arg.
 */
static void *producer_thread(void *arg)
{
    struct worker *w = arg;
    pin_to(producer_cpu(w->id));
    unsigned spins = 0;

    uint64_t t0 = now_ns();
    uint64_t cpu0 = thread_cpu_ns();
    for (uint64_t seq = 0; seq < w->count; seq++) {
        /* The key carries the producer id in the high byte so a consumer-side
         * check could tell the streams apart. */
        uint64_t key = seq | ((uint64_t)w->id << 56);

        while (!q_store(w->q, key)) {
            w->retries++;
            backoff(&spins);
        }
        spins = 0;
    }
    w->cpu_ns = thread_cpu_ns() - cpu0;
    w->elapsed_ns = now_ns() - t0;

    /* Release, so the consumer's acquire load of the counter reaching zero
     * also guarantees it sees every submitted message. */
    atomic_fetch_sub_explicit(w->producers_left, 1, memory_order_release);

    record_ctxsw(w);
    return NULL;
}

/**
 * consumer_thread - pin, then drain until every producer is done
 * @arg: struct worker
 *
 * A false pop never means "drained": the oldest slot may be reserved but
 * unsubmitted, which blocks delivery head-of-line. So the consumer waits for
 * producers_left to reach zero, then takes DRAIN_SPINS further failures.
 *
 * Return: NULL always; results are reported through @arg.
 */
static void *consumer_thread(void *arg)
{
    struct worker *w = arg;
    pin_to(CONSUMER_CPU);
    char out[SLOT_SIZE];
    unsigned idle = 0;
    unsigned spins = 0;

    uint64_t cpu0 = thread_cpu_ns();
    for (;;) {
        if (ringbuf_zcopy_pop(w->q->zc, out)) {
            w->received++;
            idle = 0;
            spins = 0;
            continue;
        }

        /* Retries count only while producers run, so the metric stays a
         * contention signal and not end-of-run polling. */
        if (atomic_load_explicit(w->producers_left, memory_order_acquire)) {
            w->retries++;
            backoff(&spins);
            continue;
        }
        if (++idle > DRAIN_SPINS)
            break;
        cpu_relax();
    }
    w->cpu_ns = thread_cpu_ns() - cpu0;

    record_ctxsw(w);
    return NULL;
}

/* ---- Result plumbing ---- */

struct result {
    double store_per_sec;
    double recv_per_sec;
    double cpu_ns_per_rx; /* all threads, per delivered message */
    double cpu_per_wall; /* producers+1 means every core pegged */
    double push_retries_per_k;
    double pop_retries_per_k;
    double ctxsw_per_k;
    double drop_pct;
    double finish_spread;
    uint64_t received;
    bool accounted; /* received + dropped == pushed */
};

/**
 * run_one - one N-producer transfer, measured
 * @producers: number of producer threads
 * @overwrite: drop-oldest when true, drop-newest when false
 *
 * Return: the measurements for this run; a zeroed result if the queue could not
 * be created.
 */
static struct result run_one(unsigned producers, bool overwrite, bool locked)
{
    struct queue q;
    struct result r = { 0 };

    if (!q_init(&q, overwrite, locked)) {
        fprintf(stderr, "error: queue creation failed\n");
        return r;
    }

    uint64_t total = TP_MESSAGES;
    atomic_uint producers_left;
    atomic_init(&producers_left, producers);

    static struct worker prod[MAX_PRODUCERS];
    static struct worker cons;
    memset(prod, 0, sizeof(prod));
    memset(&cons, 0, sizeof(cons));

    /* Split the fixed total across the producers, so wall throughput is
     * comparable across producer counts. The remainder goes to the first few.
     */
    for (unsigned i = 0; i < producers; i++) {
        prod[i].q = &q;
        prod[i].id = i;
        prod[i].producers_left = &producers_left;
        prod[i].count = total / producers + (i < total % producers);
    }
    cons.q = &q;
    cons.producers_left = &producers_left;

    pthread_t pt[MAX_PRODUCERS];
    pthread_t ct;
    uint64_t t0 = now_ns();
    for (unsigned i = 0; i < producers; i++)
        pthread_create(&pt[i], NULL, producer_thread, &prod[i]);
    pthread_create(&ct, NULL, consumer_thread, &cons);
    for (unsigned i = 0; i < producers; i++)
        pthread_join(pt[i], NULL);
    pthread_join(ct, NULL);
    uint64_t t1 = now_ns();

    uint64_t wall = t1 - t0;
    uint64_t prod_cpu = 0;
    uint64_t prod_retries = 0;
    uint64_t ctxsw = cons.vol_ctxsw + cons.invol_ctxsw;
    uint64_t slowest = 0;
    uint64_t fastest = UINT64_MAX;

    for (unsigned i = 0; i < producers; i++) {
        prod_cpu += prod[i].cpu_ns;
        prod_retries += prod[i].retries;
        ctxsw += prod[i].vol_ctxsw + prod[i].invol_ctxsw;
        if (prod[i].elapsed_ns > slowest)
            slowest = prod[i].elapsed_ns;
        if (prod[i].elapsed_ns < fastest)
            fastest = prod[i].elapsed_ns;
    }

    size_t dropped = ringbuf_zcopy_dropped_count(q.zc);
    double msgs = (double)total;
    double k = msgs / 1000.0;

    r.store_per_sec = msgs * 1e9 / (double)wall;
    r.recv_per_sec = (double)cons.received * 1e9 / (double)wall;
    r.cpu_ns_per_rx = cons.received ? (double)(prod_cpu + cons.cpu_ns) /
                                          (double)cons.received :
                                      0.0;
    r.cpu_per_wall = (double)(prod_cpu + cons.cpu_ns) / (double)wall;
    r.push_retries_per_k = (double)prod_retries / k;
    r.pop_retries_per_k = (double)cons.retries / k;
    r.ctxsw_per_k = (double)ctxsw / k;
    r.drop_pct = 100.0 * (double)dropped / msgs;
    r.finish_spread = fastest ? (double)slowest / (double)fastest : 0.0;
    r.received = cons.received;
    r.accounted = cons.received + dropped == total;

    q_destroy(&q);
    return r;
}

/**
 * bench_one - run REPS transfers and keep the median-throughput one
 * @producers: number of producer threads
 * @overwrite: drop-oldest when true, drop-newest when false
 *
 * Return: the one repetition whose store rate was the median, so every column
 * describes the same run.
 */
static struct result bench_one(unsigned producers, bool overwrite, bool locked)
{
    struct result all[REPS];
    double rate[REPS];

    for (int i = 0; i < REPS; i++) {
        all[i] = run_one(producers, overwrite, locked);
        rate[i] = all[i].store_per_sec;
    }
    return all[median_index(rate, REPS)];
}

/* ---- Output ---- */

/**
 * print_header - column headings for the sweep table
 * @overwrite: add the drop and received columns for the overwrite table
 */
static void print_header(bool overwrite)
{
    printf("  %9s %9s %9s %8s %9s %8s %9s %8s %7s", "producers", "store-Mm/s",
           "recv-Mm/s", "cpu-ns/rx", "cpu/wall", "stor-rt/k", "pop-rt/k",
           "ctxsw/k", "spread");
    if (overwrite)
        printf(" %9s %10s", "dropped%", "received");
    printf("\n");
}

/**
 * print_row - one measured row
 * @label: producer count, or a tag for a special row
 * @r: measurements to print
 * @overwrite: append the drop percentage and received count to the row
 */
static void print_row(const char *label, const struct result *r, bool overwrite)
{
    printf("  %9s %9.2f %9.2f %9.1f %8.2f %9.1f %8.1f %8.2f %7.2f", label,
           r->store_per_sec / 1e6, r->recv_per_sec / 1e6, r->cpu_ns_per_rx,
           r->cpu_per_wall, r->push_retries_per_k, r->pop_retries_per_k,
           r->ctxsw_per_k, r->finish_spread);
    if (overwrite)
        printf(" %9.2f %10" PRIu64, r->drop_pct, r->received);
    printf("%s\n", r->accounted ? "" : "  !! received + dropped != pushed");
}

/**
 * main - sweep producer counts for lossless and overwrite modes
 *
 * Return: 0 always; this is a measurement, not a pass/fail check.
 */
int main(void)
{
    size_t n = sizeof prod_counts / sizeof *prod_counts;

    g_ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (g_ncpu < 1)
        g_ncpu = 1;
    pin_to(CONSUMER_CPU);

    printf("ringbuf_zcopy+mu, multi-producer sweep, %d cpus online\n", g_ncpu);
    printf("slot=%dB, cap=%d, %u messages per run split across the producers,"
           " median of %d runs\n",
           SLOT_SIZE, CAPACITY, TP_MESSAGES, REPS);
    printf("consumer pinned to cpu %d, producers to cpus 1..%d (cycling)\n",
           CONSUMER_CPU, g_ncpu - 1);
    if (prod_counts[n - 1] > (unsigned)g_ncpu)
        printf("NOTE: the widest row (%u producers) exceeds the %d online cpus"
               " and is oversubscribed.\n",
               prod_counts[n - 1], g_ncpu);

    for (unsigned m = 0; m < 2; m++) {
        bool overwrite = m == 1;
        printf("\n%s\n", overwrite ? "[overwrite]" : "[lossless]");
        print_header(overwrite);
        for (size_t p = 0; p < n; p++) {
            char label[16];
            snprintf(label, sizeof label, "%u", prod_counts[p]);
            struct result r =
                bench_one(prod_counts[p], overwrite, prod_counts[p] > 1);
            print_row(label, &r, overwrite);
        }
        if (!overwrite) {
            /* Control: one producer, lock taken, to price the uncontended mutex. */
            struct result r = bench_one(1, false, true);
            print_row("1+lock", &r, false);
        }
    }

    return 0;
}
