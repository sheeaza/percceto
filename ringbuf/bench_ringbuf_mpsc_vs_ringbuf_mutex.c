/*
 * Head-to-head benchmark: ringbuf_mpsc against ringbuf_mutex guarded by a
 * caller-supplied producer mutex.
 *
 * bench_ringbuf_vs_ringbuf_mutex.c compares two variants that accept the
 * same SPSC contract, so the only difference is how overwrite mode is
 * synchronized. The two sides here do not share a contract at all:
 * ringbuf_mpsc natively accepts N concurrent producers, ringbuf_mutex
 * permits exactly one. The honest comparison is therefore against what a
 * caller would actually write if ringbuf_mutex were all they had -- wrap
 * push in their own mutex and let N threads share it:
 *
 *     pthread_mutex_lock(&push_lock);
 *     ok = ringbuf_mutex_push(rb, msg);
 *     pthread_mutex_unlock(&push_lock);
 *
 * That is legal, and the reason is worth stating because it is the premise
 * of the whole file. The SPSC contract forbids CONCURRENT pushers, not a
 * rotating producer identity. All producer-private state inside
 * ringbuf_mutex -- cached_read, and the release store to write_idx -- is
 * handed from one producer thread to the next through that mutex's own
 * acquire/release edges, so one-at-a-time from many threads is as safe as
 * always-the-same-thread. The consumer never takes push_lock: only
 * producers contend for it, which is exactly the cost being priced.
 *
 * Two deliberate choices in that wrapper:
 *
 *   - The retry loop lives OUTSIDE push_lock. Spinning on a full buffer
 *     while holding it would block every other producer behind a condition
 *     only the consumer can clear, which is a bug in the wrapper rather
 *     than a property of ringbuf_mutex.
 *   - push_lock sits in its own cache line, so the comparison is not
 *     measuring false sharing between the lock and the queue pointer.
 *
 * What the columns measure is the same as the other head-to-head: wall
 * throughput, CPU-nanoseconds per delivered message summed over all
 * threads, CPU-to-wall ratio, retry counts, and context switches. Each row
 * reports the single repetition whose throughput was the median, so its
 * columns describe one coherent run rather than medians taken per column.
 *
 * Two things differ from that file and the numbers are NOT comparable
 * across the two:
 *
 *   - cpu/wall saturates near producers+1, not 2.0.
 *   - A failed push or pop backs off with sched_yield() after YIELD_SPINS
 *     consecutive failures instead of spinning on cpu_relax() forever. The
 *     oversubscribed scenario runs more producers than there are cores, and
 *     there a pure spin is not a measurement: ringbuf_mpsc can have a
 *     producer holding a ticket it has not published yet, which blocks
 *     delivery head-of-line until that thread is scheduled again. Yielding
 *     is what a real caller would do, it is applied identically to both
 *     variants, and on a thread pinned to its own idle CPU it costs
 *     essentially nothing -- so it does not distort the undersubscribed
 *     rows.
 *
 * The consumer is pinned to one CPU and each producer to another, cycling
 * over the remaining CPUs; an unpinned mid-run migration shows up as a
 * throughput cliff unrelated to the code.
 */

#define _GNU_SOURCE

#include <errno.h>
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

#include "ringbuf_mpsc.h"
#include "ringbuf_mutex.h"

#define REPS 5
#define TP_MESSAGES 2000000u

/* Upper bound on concurrent producers, sized for the oversubscribed
 * scenario rather than for the core count. */
#define MAX_PRODUCERS 16

/* Failed pops tolerated after the last producer signals done before the
 * consumer calls it empty. A single retry is not enough in either variant:
 * ringbuf_mpsc returns false on head-of-line waits and on slot-lock
 * contention, and breaking on the first false would undercount received and
 * break the received + dropped == pushed check. */
#define DRAIN_SPINS 10000u

/* Consecutive failures before a retry loop yields instead of spinning.
 * See the file comment: this is what keeps the oversubscribed scenario a
 * measurement rather than a thundering-herd artifact. */
#define YIELD_SPINS 64u

/* ---- Paced scenario parameters ----
 *
 * In the saturated scenarios every thread spins flat out, so cpu/wall pins
 * near its ceiling and CPU-per-message degenerates into a restatement of
 * throughput. The paced scenario fixes the offered rate below what either
 * variant can sustain and lets all sides idle, which is the only way the
 * CPU columns carry information the throughput column does not.
 *
 * Each producer sends PACE_BURST messages every PACE_BURST_NS, sleeping in
 * between, rather than pacing each message individually -- a per-message
 * sleep at this rate would be dominated by the sleep syscall itself. The
 * consumer backs off into a sleep after PACE_SPINS failed pops. Both
 * behaviors are identical for the two variants, so their cost cancels. */
#define PACE_MESSAGES 1000000u
#define PACE_BURST 100u
#define PACE_BURST_NS 100000u
#define PACE_SPINS 200u
#define PACE_BACKOFF_NS 50000u

/*
 * VARIANT_MUTEX_RAW calls ringbuf_mutex_push() with no external lock at
 * all. It is valid ONLY in the single-threaded scenario, where it prices
 * the external mutex by difference; using it with more than one producer
 * would be undefined behavior, not a slow result.
 */
enum variant { VARIANT_MPSC, VARIANT_MUTEX_MU, VARIANT_MUTEX_RAW };

/**
 * variant_name - printable name of an implementation
 * @v: which implementation
 *
 * Return: a static string, never NULL.
 */
static const char *variant_name(enum variant v)
{
    switch (v) {
    case VARIANT_MPSC:
        return "ringbuf_mpsc";
    case VARIANT_MUTEX_MU:
        return "ringbuf_mutex+mu";
    default:
        return "ringbuf_mutex raw";
    }
}

/*
 * One scenario cell: everything that varies between measured rows. Passed
 * by pointer rather than as eight positional arguments, since several of
 * them are bools and sizes that would be easy to transpose at a call site.
 */
struct scenario {
    enum variant variant;
    bool overwrite;
    size_t slot_size;
    size_t capacity; /* lower bound; the ring rounds it up */
    unsigned producers;
    unsigned consumer_work; /* dummy work units the consumer does per msg */
    bool paced;
};

/* CPU topology, filled in once by main(). */
static int g_ncpu = 1;

#define CONSUMER_CPU 0

/**
 * producer_cpu - CPU to pin producer @i to
 * @i: producer index
 *
 * Producers take the CPUs above the consumer's, cycling once they run out.
 * Cycling rather than refusing is what makes the oversubscribed scenario
 * expressible on any host: with more producers than CPUs, several share a
 * CPU deterministically instead of being left to the scheduler.
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
 *
 * Falls back to a relaxed fence on non-x86, which at least keeps the
 * compiler from hoisting the load out of the spin loop.
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
 * backoff - pause after a failed push or pop, yielding once it persists
 * @spins: caller's consecutive-failure counter, updated in place
 *
 * Spins for the first YIELD_SPINS failures and yields after that. Reset
 * @spins to 0 on success. See the file comment for why the yield is here
 * and why it does not bias the undersubscribed rows.
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
 * Counts only on-CPU time, so a thread blocked on a futex accrues none of
 * it while descheduled. That is exactly the asymmetry being measured: the
 * mutex wrapper parks its losers, ringbuf_mpsc's losers burn cycles.
 *
 * Return: nanoseconds of CPU time charged to this thread.
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
 * Warns rather than failing: an unpinned run still produces numbers, they
 * are just noisier.
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
 * sleep_ns - suspend this thread for a short interval
 * @ns: nanoseconds to sleep
 *
 * Used only by the paced scenario, to give up the CPU rather than spin.
 * Retries on EINTR so a signal cannot silently shorten the pace interval.
 */
static void sleep_ns(uint64_t ns)
{
    struct timespec ts = { .tv_sec = (time_t)(ns / 1000000000u),
                           .tv_nsec = (long)(ns % 1000000000u) };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
        ;
}

/**
 * cmp_dbl - qsort comparator for double
 * @a: first element
 * @b: second element
 *
 * Return: negative, zero, or positive as *@a orders before, same as, or
 * after *@b.
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
 * Used to pick one whole repetition to report, so that every column of a
 * result row comes from the same run. Taking a per-column median instead
 * would mix runs and could print a CPU figure that never accompanied the
 * printed throughput. For even @n this returns the lower of the middle
 * pair rather than averaging, since there is no averaging a whole run.
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

/* ---- The two queues behind one interface ---- */

/*
 * Exactly one of @mpsc and @mu is non-NULL. @push_lock is the
 * caller-supplied producer mutex that makes ringbuf_mutex usable from N
 * threads; it is initialized only for VARIANT_MUTEX_MU.
 *
 * _Alignas(64) on push_lock keeps it off the line holding the queue
 * pointers, which every producer reads on every push. Without that the
 * measurement would include false sharing introduced by this harness.
 */
struct queue {
    enum variant variant;
    struct ringbuf_mpsc *mpsc;
    struct ringbuf_mutex *mu;
    _Alignas(64) pthread_mutex_t push_lock;
};

/**
 * q_init - create the queue a scenario calls for
 * @q: queue to initialize
 * @s: scenario describing which implementation and geometry
 *
 * Return: true on success; false if allocation or mutex init failed, in
 * which case @q needs no teardown.
 */
static bool q_init(struct queue *q, const struct scenario *s)
{
    memset(q, 0, sizeof(*q));
    q->variant = s->variant;

    if (s->variant == VARIANT_MPSC) {
        q->mpsc = ringbuf_mpsc_create(s->slot_size, s->capacity, s->overwrite);
        return q->mpsc != NULL;
    }

    q->mu = ringbuf_mutex_create(s->slot_size, s->capacity, s->overwrite);
    if (!q->mu)
        return false;
    if (s->variant == VARIANT_MUTEX_MU &&
        pthread_mutex_init(&q->push_lock, NULL) != 0) {
        ringbuf_mutex_destroy(q->mu);
        q->mu = NULL;
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
    if (q->mpsc)
        ringbuf_mpsc_destroy(q->mpsc);
    if (q->mu) {
        if (q->variant == VARIANT_MUTEX_MU)
            pthread_mutex_destroy(&q->push_lock);
        ringbuf_mutex_destroy(q->mu);
    }
}

/**
 * q_push - copy one message in, from any producer thread
 * @q: the queue
 * @data: slot_size bytes to copy in
 *
 * For VARIANT_MUTEX_MU this is the whole point of the file: one attempt
 * under the external mutex, with the retry left to the caller so a full
 * buffer cannot block the other producers.
 *
 * Return: true if stored; false if full, or on transient contention in the
 * lock-free variant's overwrite mode.
 */
static bool q_push(struct queue *q, const void *data)
{
    if (q->variant == VARIANT_MPSC)
        return ringbuf_mpsc_push(q->mpsc, data);
    if (q->variant == VARIANT_MUTEX_RAW)
        return ringbuf_mutex_push(q->mu, data);

    pthread_mutex_lock(&q->push_lock);
    bool ok = ringbuf_mutex_push(q->mu, data);
    pthread_mutex_unlock(&q->push_lock);
    return ok;
}

/**
 * q_pop - copy one message out
 * @q: the queue
 * @data: destination for slot_size bytes
 *
 * Never takes push_lock: the consumer contends only with whatever the
 * implementation does internally, which is the asymmetry under test.
 *
 * Return: true if a message was copied out; false if empty, or -- in the
 * MPSC variant -- on a head-of-line wait or slot-lock contention.
 */
static bool q_pop(struct queue *q, void *data)
{
    if (q->variant == VARIANT_MPSC)
        return ringbuf_mpsc_pop(q->mpsc, data);
    return ringbuf_mutex_pop(q->mu, data);
}

/**
 * q_dropped - eviction count
 * @q: the queue
 *
 * Return: messages evicted before they were read; 0 in lossless mode.
 */
static size_t q_dropped(const struct queue *q)
{
    if (q->variant == VARIANT_MPSC)
        return ringbuf_mpsc_dropped_count(q->mpsc);
    return ringbuf_mutex_dropped_count(q->mu);
}

/* ---- Worker threads ---- */

/*
 * _Alignas(64) on the first member keeps each worker on its own cache
 * line(s) -- it raises the struct's alignment, which also pads sizeof up to
 * a multiple of 64, so an array of them stays line-separated. (It has to
 * sit on a member: C11 does not accept an alignment specifier on a struct
 * tag.) Every thread writes its own instance on every iteration, so sharing
 * a line would inject false sharing into the thing being measured. The
 * per-thread `sink` lives here for the same reason -- a single global sink
 * written from several cores is itself a coherence benchmark.
 */
struct worker {
    _Alignas(64) struct queue *q;
    size_t slot_size;
    unsigned id;
    uint64_t count; /* producer: messages to send */
    unsigned consumer_work;
    bool paced;
    atomic_uint *producers_left;

    /* Written by the owning thread only. */
    volatile uint64_t sink;
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
 *
 * RUSAGE_THREAD is Linux-specific, hence the _GNU_SOURCE at the top. The
 * voluntary count is the interesting one: it is where blocking on the
 * external mutex's futex shows up, and it is the cost ringbuf_mpsc trades
 * for spin cycles.
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
 * producer_thread - pin, then push count sequenced messages
 * @arg: struct worker
 *
 * Counts its own retries, CPU time, and wall time. The wall time feeds the
 * finish-spread figure: with N producers sharing one queue, how evenly the
 * N shares complete is a property of the synchronization, and a mutex that
 * barges is visibly less even than one that does not.
 *
 * When @paced, sends in bursts of PACE_BURST and sleeps to hold the offered
 * rate below saturation, so the CPU columns measure something other than
 * spinning.
 *
 * Return: NULL always; results are reported through @arg.
 */
static void *producer_thread(void *arg)
{
    struct worker *w = arg;
    pin_to(producer_cpu(w->id));
    char *msg = calloc(1, w->slot_size);
    uint64_t next_burst = now_ns();
    unsigned spins = 0;

    uint64_t t0 = now_ns();
    uint64_t cpu0 = thread_cpu_ns();
    for (uint64_t seq = 0; seq < w->count; seq++) {
        if (w->paced && seq % PACE_BURST == 0) {
            next_burst += PACE_BURST_NS;
            uint64_t t = now_ns();
            if (next_burst > t)
                sleep_ns(next_burst - t);
        }
        /* The payload carries the producer id in the high byte so a
         * consumer-side check could tell the streams apart; the consumer
         * here only sums it, but the store has to happen either way. */
        uint64_t payload = seq | ((uint64_t)w->id << 56);
        memcpy(msg, &payload, sizeof(payload));
        while (!q_push(w->q, msg)) {
            w->retries++;
            backoff(&spins);
        }
        spins = 0;
    }
    w->cpu_ns = thread_cpu_ns() - cpu0;
    w->elapsed_ns = now_ns() - t0;

    /* Release, so the consumer's acquire load of the counter reaching zero
     * also guarantees it sees every published message. */
    atomic_fetch_sub_explicit(w->producers_left, 1, memory_order_release);

    record_ctxsw(w);
    free(msg);
    return NULL;
}

/**
 * consumer_thread - pin, then drain until every producer is done
 * @arg: struct worker
 *
 * Optionally does @consumer_work units of dummy dependent work per message,
 * to simulate a consumer that cannot keep up. That is what drives the
 * buffer permanently full and makes overwrite-mode eviction the common case
 * rather than a rarity.
 *
 * When @paced, backs off into a sleep after PACE_SPINS consecutive failed
 * pops instead of spinning, so an idle consumer costs wall time rather than
 * burning CPU.
 *
 * Return: NULL always; results are reported through @arg.
 */
static void *consumer_thread(void *arg)
{
    struct worker *w = arg;
    pin_to(CONSUMER_CPU);
    char *out = calloc(1, w->slot_size);
    unsigned idle = 0;
    unsigned miss = 0;
    unsigned spins = 0;

    uint64_t cpu0 = thread_cpu_ns();
    for (;;) {
        if (q_pop(w->q, out)) {
            uint64_t payload;
            memcpy(&payload, out, sizeof(payload));
            w->sink += payload;
            /* Volatile, so each iteration is a real load/store the
             * optimizer cannot fold away or hoist out. */
            for (unsigned i = 0; i < w->consumer_work; i++)
                w->sink += (w->sink >> 3) | 1u;
            w->received++;
            idle = 0;
            miss = 0;
            spins = 0;
            continue;
        }

        /* Retries are counted only while producers are still running, so
         * the metric stays a contention signal and does not absorb up to
         * DRAIN_SPINS of end-of-run polling. */
        if (atomic_load_explicit(w->producers_left, memory_order_acquire)) {
            w->retries++;
            if (w->paced && ++miss >= PACE_SPINS) {
                miss = 0;
                sleep_ns(PACE_BACKOFF_NS);
            } else {
                backoff(&spins);
            }
            continue;
        }
        if (++idle > DRAIN_SPINS)
            break;
        cpu_relax();
    }
    w->cpu_ns = thread_cpu_ns() - cpu0;

    record_ctxsw(w);
    free(out);
    return NULL;
}

/* ---- Result plumbing ---- */

struct result {
    double msgs_per_sec;
    /* Messages actually delivered to the consumer per second. In overwrite
     * mode msgs_per_sec counts PUSHES, and evicting is cheaper than
     * delivering -- so a variant can post a higher push rate purely by
     * dropping more. This is the metric that cannot be gamed that way, and
     * it is the one to compare when the drop percentages differ. */
    double recv_per_sec;
    double cpu_ns_per_msg; /* all threads summed */
    double cpu_per_wall; /* producers+1 means every core pegged */
    double prod_cpu_ns_per_msg; /* all producers summed */
    double cons_cpu_ns_per_msg;
    double push_retries_per_k;
    double pop_retries_per_k;
    double ctxsw_per_k; /* voluntary + involuntary, every thread */
    double drop_pct;
    double finish_spread; /* slowest producer / fastest, 1.0 is perfect */
    uint64_t received;
    uint64_t dropped;
    bool accounted; /* received + dropped == pushed */
};

/**
 * run_one - one N-producer transfer, measured
 * @s: scenario to run
 *
 * Return: the measurements for this single run; a zeroed result if the
 * queue could not be created.
 */
static struct result run_one(const struct scenario *s)
{
    struct queue q;
    struct result r = { 0 };

    if (!q_init(&q, s)) {
        fprintf(stderr, "error: queue creation failed\n");
        return r;
    }

    uint64_t total = s->paced ? PACE_MESSAGES : TP_MESSAGES;
    atomic_uint producers_left;
    atomic_init(&producers_left, s->producers);

    static struct worker prod[MAX_PRODUCERS];
    static struct worker cons;
    memset(prod, 0, sizeof(prod));
    memset(&cons, 0, sizeof(cons));

    /* Split the fixed total across the producers, so wall throughput stays
     * comparable as the producer count changes. The remainder goes to the
     * first few rather than being dropped. */
    for (unsigned i = 0; i < s->producers; i++) {
        prod[i].q = &q;
        prod[i].slot_size = s->slot_size;
        prod[i].id = i;
        prod[i].paced = s->paced;
        prod[i].producers_left = &producers_left;
        prod[i].count = total / s->producers + (i < total % s->producers);
    }
    cons.q = &q;
    cons.slot_size = s->slot_size;
    cons.consumer_work = s->consumer_work;
    cons.paced = s->paced;
    cons.producers_left = &producers_left;

    pthread_t pt[MAX_PRODUCERS];
    pthread_t ct;
    uint64_t t0 = now_ns();
    for (unsigned i = 0; i < s->producers; i++)
        pthread_create(&pt[i], NULL, producer_thread, &prod[i]);
    pthread_create(&ct, NULL, consumer_thread, &cons);
    for (unsigned i = 0; i < s->producers; i++)
        pthread_join(pt[i], NULL);
    pthread_join(ct, NULL);
    uint64_t t1 = now_ns();

    uint64_t wall = t1 - t0;
    uint64_t prod_cpu = 0;
    uint64_t prod_retries = 0;
    uint64_t ctxsw = cons.vol_ctxsw + cons.invol_ctxsw;
    uint64_t slowest = 0;
    uint64_t fastest = UINT64_MAX;

    for (unsigned i = 0; i < s->producers; i++) {
        prod_cpu += prod[i].cpu_ns;
        prod_retries += prod[i].retries;
        ctxsw += prod[i].vol_ctxsw + prod[i].invol_ctxsw;
        if (prod[i].elapsed_ns > slowest)
            slowest = prod[i].elapsed_ns;
        if (prod[i].elapsed_ns < fastest)
            fastest = prod[i].elapsed_ns;
    }

    uint64_t dropped = q_dropped(&q);
    double msgs = (double)total;
    double k = msgs / 1000.0;

    r.msgs_per_sec = msgs * 1e9 / (double)wall;
    r.recv_per_sec = (double)cons.received * 1e9 / (double)wall;
    r.cpu_ns_per_msg = (double)(prod_cpu + cons.cpu_ns) / msgs;
    r.cpu_per_wall = (double)(prod_cpu + cons.cpu_ns) / (double)wall;
    r.prod_cpu_ns_per_msg = (double)prod_cpu / msgs;
    r.cons_cpu_ns_per_msg = (double)cons.cpu_ns / msgs;
    r.push_retries_per_k = (double)prod_retries / k;
    r.pop_retries_per_k = (double)cons.retries / k;
    r.ctxsw_per_k = (double)ctxsw / k;
    r.drop_pct = 100.0 * (double)dropped / msgs;
    r.finish_spread = fastest ? (double)slowest / (double)fastest : 0.0;
    r.received = cons.received;
    r.dropped = dropped;
    r.accounted = cons.received + dropped == total;

    q_destroy(&q);
    return r;
}

/**
 * bench_one - run REPS transfers and keep the median-throughput one
 * @s: scenario to run
 *
 * Return: the one repetition whose throughput was the median, so that all
 * of its columns describe the same run.
 */
static struct result bench_one(const struct scenario *s)
{
    struct result all[REPS];
    double rate[REPS];

    for (int i = 0; i < REPS; i++) {
        all[i] = run_one(s);
        rate[i] = all[i].msgs_per_sec;
    }
    return all[median_index(rate, REPS)];
}

/**
 * bench_single - time alternating push/pop on one thread
 * @s: scenario to run; its producers and paced fields are ignored
 * @cpu_ns_per_op: out-param for CPU nanoseconds per push+pop pair
 *
 * Queue depth stays at 1, so there is no cross-core coherence traffic and
 * no contention at all: this isolates the uncontended instruction cost of
 * each implementation's synchronization. Running VARIANT_MUTEX_RAW and
 * VARIANT_MUTEX_MU side by side here is what prices the external mutex on
 * its own, separately from any contention for it.
 *
 * Return: median messages/sec over REPS repetitions, where one message is
 * a full push+pop pair.
 */
static double bench_single(const struct scenario *s, double *cpu_ns_per_op)
{
    double rate[REPS];
    double cpu[REPS];
    char *msg = calloc(1, s->slot_size);
    char *out = calloc(1, s->slot_size);

    for (int i = 0; i < REPS; i++) {
        struct queue q;
        if (!q_init(&q, s)) {
            fprintf(stderr, "error: queue creation failed\n");
            rate[i] = 0;
            cpu[i] = 0;
            continue;
        }

        /* Warm the slots so the timed loop is not measuring page faults. */
        for (size_t j = 0; j < s->capacity; j++) {
            q_push(&q, msg);
            q_pop(&q, out);
        }

        uint64_t c0 = thread_cpu_ns();
        uint64_t t0 = now_ns();
        for (unsigned n = 0; n < TP_MESSAGES; n++) {
            memcpy(msg, &n, sizeof(n));
            while (!q_push(&q, msg))
                cpu_relax();
            while (!q_pop(&q, out))
                cpu_relax();
        }
        uint64_t t1 = now_ns();
        uint64_t c1 = thread_cpu_ns();

        rate[i] = (double)TP_MESSAGES * 1e9 / (double)(t1 - t0);
        cpu[i] = (double)(c1 - c0) / (double)TP_MESSAGES;
        q_destroy(&q);
    }

    free(msg);
    free(out);

    int m = median_index(rate, REPS);
    *cpu_ns_per_op = cpu[m];
    return rate[m];
}

/* ---- Output ---- */

/**
 * print_header - column headings for the N-producer tables
 *
 * push-Mm/s is pushes completed per second; recv-Mm/s is messages actually
 * delivered. They differ only in overwrite mode, and that gap is the point:
 * see struct result's recv_per_sec.
 */
static void print_header(void)
{
    printf("  %-17s %9s %9s %9s %8s %8s %8s %9s %8s %7s\n", "impl", "push-Mm/s",
           "recv-Mm/s", "cpu-ns/rx", "cpu/wall", "prod-ns", "cons-ns",
           "push-rt/k", "pop-rt/k", "ctxsw/k");
}

/**
 * print_row - one measured row, plus the second line it needs
 * @label: row label, normally the implementation name
 * @r: measurements to print
 * @show_drop: include the eviction percentage (overwrite mode only)
 *
 * The CPU column is normalized per DELIVERED message, not per push, so it
 * cannot be improved by dropping more; prod-ns and cons-ns stay per push,
 * since that is the work those threads actually performed.
 *
 * The second line carries the figures that do not fit the fixed columns:
 * eviction share where it applies, and the producer finish spread, which is
 * the fairness signal and only means anything with more than one producer.
 */
static void print_row(const char *label, const struct result *r, bool show_drop)
{
    double cpu_per_rx =
        r->received ? r->cpu_ns_per_msg * r->msgs_per_sec / r->recv_per_sec :
                      0.0;

    printf("  %-17s %9.2f %9.2f %9.1f %8.2f %8.1f %8.1f %9.1f %8.1f %7.2f\n",
           label, r->msgs_per_sec / 1e6, r->recv_per_sec / 1e6, cpu_per_rx,
           r->cpu_per_wall, r->prod_cpu_ns_per_msg, r->cons_cpu_ns_per_msg,
           r->push_retries_per_k, r->pop_retries_per_k, r->ctxsw_per_k);

    printf("  %-17s  ", "");
    if (show_drop)
        printf("dropped %.2f%%, received %" PRIu64 ", ", r->drop_pct,
               r->received);
    printf("finish spread %.2fx%s\n", r->finish_spread,
           r->accounted ? "" : "  !! received + dropped != pushed");
}

/**
 * bench_pair - run both implementations for one cell and print both rows
 * @proto: scenario to run; its variant field is overwritten
 *
 * Every table in main() is this shape -- the two implementations under
 * identical conditions -- so the loop lives here rather than being repeated
 * six times.
 */
static void bench_pair(const struct scenario *proto)
{
    static const enum variant variants[] = { VARIANT_MPSC, VARIANT_MUTEX_MU };

    print_header();
    for (unsigned i = 0; i < sizeof variants / sizeof *variants; i++) {
        struct scenario s = *proto;
        s.variant = variants[i];
        struct result r = bench_one(&s);
        print_row(variant_name(variants[i]), &r, proto->overwrite);
    }
}

/**
 * main - run every comparison and print the tables
 *
 * Takes no arguments: the scenario set and its parameters are fixed at
 * compile time.
 *
 * Return: 0 always; this is a measurement, not a pass/fail check.
 */
int main(void)
{
    static const unsigned prod_counts[] = { 1, 2, 4, 7 };
    static const size_t caps[] = { 16, 256, 4096, 65536 };
    static const size_t sizes[] = { 16, 64, 256, 1024 };

    g_ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (g_ncpu < 1)
        g_ncpu = 1;
    pin_to(CONSUMER_CPU);

    printf("ringbuf_mpsc vs ringbuf_mutex under a caller-held push mutex\n");
    printf("%u messages per run split across the producers, median of %d"
           " runs\n",
           TP_MESSAGES, REPS);
    printf("consumer pinned to cpu %d, producers to cpus 1..%d (cycling);"
           " %d cpus online\n",
           CONSUMER_CPU, g_ncpu - 1, g_ncpu);
    printf("ringbuf_mutex is SPSC, so its producers serialize on an external"
           " pthread_mutex_t:\n  lock / one push attempt / unlock, with the"
           " retry outside the lock.\n");
    printf("cpu/wall saturates near producers+1 here, not 2.0, so these"
           " numbers are not\ncomparable with"
           " bench_ringbuf_vs_ringbuf_mutex.\n");
    printf("In overwrite mode compare recv-Mm/s, not push-Mm/s: evicting is"
           " cheaper than\ndelivering, so a variant can post a higher push"
           " rate just by dropping more.\n");
    if (prod_counts[sizeof prod_counts / sizeof *prod_counts - 1] >
        (unsigned)(g_ncpu - 1))
        printf("NOTE: fewer cpus than the widest scenario asks for; the wide"
               " rows share cores\n  and are oversubscribed like scenario"
               " [6].\n");

    printf("\n[1] Single thread, alternating push/pop (depth 1), 64B slots,"
           " cap 1024\n");
    printf("    Uncontended cost, and the price of the external mutex on its"
           " own: the raw\n    row takes no push lock (legal only because"
           " there is one thread here), so the\n    gap to +mu is one"
           " uncontended lock/unlock pair.\n");
    printf("  %-17s %10s %8s %10s\n", "impl", "mode", "Mmsg/s", "cpu-ns/op");
    for (unsigned m = 0; m < 2; m++) {
        static const enum variant single[] = { VARIANT_MPSC, VARIANT_MUTEX_MU,
                                               VARIANT_MUTEX_RAW };
        for (unsigned i = 0; i < sizeof single / sizeof *single; i++) {
            struct scenario s = { .variant = single[i],
                                  .overwrite = m == 1,
                                  .slot_size = 64,
                                  .capacity = 1024,
                                  .producers = 1 };
            double cpu_op = 0;
            double rate = bench_single(&s, &cpu_op);
            printf("  %-17s %10s %8.2f %10.2f\n", variant_name(single[i]),
                   s.overwrite ? "overwrite" : "lossless", rate / 1e6, cpu_op);
        }
    }

    printf("\n[2] Lossless, 64B slots, cap 4096, by producer count\n");
    printf("    The headline comparison. One producer is the control: there"
           " the external\n    mutex is uncontended and the ticket CAS is"
           " uncontested, so any gap is\n    instruction cost. Past that,"
           " this is lock-free fan-in against serialized\n    fan-in.\n");
    for (unsigned p = 0; p < sizeof prod_counts / sizeof *prod_counts; p++) {
        printf("  producers=%u\n", prod_counts[p]);
        struct scenario s = { .overwrite = false,
                              .slot_size = 64,
                              .capacity = 4096,
                              .producers = prod_counts[p] };
        bench_pair(&s);
    }

    printf("\n[3] Overwrite, 64B slots, cap 4096, by producer count\n");
    printf("    Same sweep with eviction in play, which adds a second"
           " contention point:\n    ringbuf_mpsc's evicting producers take a"
           " slot lock and can fail, while\n    ringbuf_mutex's take its"
           " internal mutex on top of the external one.\n");
    for (unsigned p = 0; p < sizeof prod_counts / sizeof *prod_counts; p++) {
        printf("  producers=%u\n", prod_counts[p]);
        struct scenario s = { .overwrite = true,
                              .slot_size = 64,
                              .capacity = 4096,
                              .producers = prod_counts[p] };
        bench_pair(&s);
    }

    printf("\n[4] Overwrite, 4 producers, 64B slots, by capacity\n");
    printf("    Small capacities fill constantly, so the eviction paths are"
           " hit hard and\n    head-of-line stalls on an unpublished ticket"
           " get more likely; large ones\n    rarely fill at all and isolate"
           " the fan-in cost.\n");
    for (unsigned c = 0; c < sizeof caps / sizeof *caps; c++) {
        printf("  cap=%zu\n", caps[c]);
        struct scenario s = { .overwrite = true,
                              .slot_size = 64,
                              .capacity = caps[c],
                              .producers = 4 };
        bench_pair(&s);
    }

    printf("\n[5] Lossless, 4 producers, cap 4096, by slot size\n");
    printf("    Checks that the synchronization gap does not simply vanish"
           " under memcpy\n    cost as payloads grow. Lossless, so the only"
           " contention is the fan-in.\n");
    for (unsigned z = 0; z < sizeof sizes / sizeof *sizes; z++) {
        printf("  slot=%zuB\n", sizes[z]);
        struct scenario s = { .overwrite = false,
                              .slot_size = sizes[z],
                              .capacity = 4096,
                              .producers = 4 };
        bench_pair(&s);
    }

    printf("\n[6] Oversubscribed: %u producers on %d producer cpus\n",
           MAX_PRODUCERS, g_ncpu - 1);
    printf("    The scenario that should favor the mutex. A descheduled"
           " producer holding an\n    unpublished ticket blocks ringbuf_mpsc"
           " delivery head-of-line, where a\n    descheduled producer waiting"
           " on the mutex blocks nothing. Both variants\n    yield after %u"
           " failed attempts; see the note at the top of the source.\n",
           YIELD_SPINS);
    for (unsigned m = 0; m < 2; m++) {
        printf("  %s, cap 4096\n", m ? "overwrite" : "lossless");
        struct scenario s = { .overwrite = m == 1,
                              .slot_size = 64,
                              .capacity = 4096,
                              .producers = MAX_PRODUCERS };
        bench_pair(&s);
    }

    printf("\n[7] Slow consumer, 4 producers, overwrite, 200 work units/msg\n");
    printf("    Worst case for eviction: the consumer cannot keep up, so the"
           " buffer is\n    permanently full and nearly every push evicts."
           " Consumer work inflates\n    cons-ns equally for both, so compare"
           " the two rows, not across tables.\n");
    for (unsigned c = 0; c < sizeof caps / sizeof *caps; c++) {
        if (caps[c] != 16 && caps[c] != 4096)
            continue;
        printf("  cap=%zu\n", caps[c]);
        struct scenario s = { .overwrite = true,
                              .slot_size = 64,
                              .capacity = caps[c],
                              .producers = 4,
                              .consumer_work = 200 };
        bench_pair(&s);
    }

    printf("\n[8] Paced below saturation, 4 producers, 64B slots (%u msgs,"
           " bursts of %u per %u us each)\n",
           PACE_MESSAGES, PACE_BURST, PACE_BURST_NS / 1000);
    printf("    Throughput here is set by the pacing, not the queue, so"
           " ignore Mm/s and read\n    the CPU columns: this is the only"
           " scenario where no thread spins flat out,\n    so cpu/wall is"
           " free to fall well below its ceiling and CPU per message"
           " becomes\n    a real measurement rather than a restatement of"
           " wall time.\n");
    for (unsigned m = 0; m < 2; m++) {
        printf("  %s, cap 4096\n", m ? "overwrite" : "lossless");
        struct scenario s = { .overwrite = m == 1,
                              .slot_size = 64,
                              .capacity = 4096,
                              .producers = 4,
                              .paced = true };
        bench_pair(&s);
    }

    return 0;
}
