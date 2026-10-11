/*
 * Head-to-head benchmark: ringbuf_mpsc against ringbuf_zcopy under a
 * caller-held push mutex, with both variants using their split push.
 *
 * The two variants take N producers by opposite means, and that is the whole
 * subject of this file. ringbuf_mpsc synchronizes producers itself -- the
 * write index is multi-writer and every claim goes through a validating
 * ticket CAS -- so reserve() needs no caller lock and stays lock-free and
 * async-signal-safe. ringbuf_zcopy keeps write_idx single-writer and requires
 * the CALLER to serialize every reserve with a mutex of its own:
 *
 *     pthread_mutex_lock(&push_lock);        -- serialize the CLAIM
 *     bool got = ringbuf_zcopy_reserve(rb, &rsv);
 *     pthread_mutex_unlock(&push_lock);
 *
 *     if (got) {
 *             fill(rsv.data);               -- OUTSIDE the lock, so every
 *             ringbuf_zcopy_submit(rb, &rsv);  producer's fill overlaps
 *     }
 *
 * This row is labelled ringbuf_zcopy+mu throughout. Two deliberate choices in
 * that wrapper, both required rather than stylistic:
 *
 *   - The retry loop lives OUTSIDE push_lock. A refused reserve is usually
 *     blocked by a PEER's unsubmitted slot, so spinning while holding the
 *     lock would block the very producer whose submit would clear it. That is
 *     a deadlock, not a slow result -- see ringbuf_zcopy.h rule 2.
 *   - submit is called with the lock released, for the same reason.
 *
 * WHY THIS COMPARISON NEEDED A LIBRARY CHANGE. Before reserve/submit existed
 * on ringbuf_mpsc the two sides did not share an operation: push() copies the
 * payload from a caller buffer, so a producer that GENERATES its message
 * writes the bytes twice (into staging, then into the slot), while
 * ringbuf_zcopy writes them once, in place. Any comparison would have
 * conflated "one copy vs two" with "ticket CAS vs caller mutex". No fill model
 * fixes that; it was an API difference. With the split push on both sides, the
 * headline rows do equal byte-moving work and the only remaining difference is
 * the synchronization.
 *
 * FOUR ROWS, which is what makes a gap attributable:
 *
 *   ringbuf_mpsc rsv   reserve / fill in place / submit     1 copy, no lock
 *   ringbuf_zcopy+mu   lock/reserve/unlock, fill, submit    1 copy, caller mutex
 *   ringbuf_mpsc push  fill staging, push                   2 copies, no lock
 *   ringbuf_zcopy+mu push  fill staging, lock/push/unlock   2 copies, mutex
 *                                                           held across copy
 *
 * Rows 1 vs 2 are the headline. Rows 3 vs 4 are the pre-split control, and
 * reproduce what the old comparison would have measured. Rows 1 vs 3 and 2 vs
 * 4 price the split push WITHIN each variant, which is how to tell whether an
 * advantage came from filling in place or from the synchronization.
 *
 * THE FILL-COST AXIS is the reason this benchmark can say anything at all.
 * Filling a slot is the work the two arrangements overlap differently: the
 * mutex row covers only the claim, so N fills proceed concurrently, while the
 * `push` rows necessarily serialize the copy inside whatever lock they hold.
 * With a trivial fill there is nothing to overlap and the mutex is pure
 * overhead; with an expensive one the overlap is the whole game. fill() does
 * `passes` word-wise read-modify-write passes over the payload through a
 * volatile pointer, so its cost scales with slot_size * passes and cannot be
 * optimized away, and scenario [4] sweeps it.
 *
 * Measured columns are the same as bench_ringbuf_mpsc_vs_ringbuf_mutex.c:
 * wall throughput, CPU-nanoseconds per delivered message summed over all
 * threads, CPU-to-wall ratio, retry counts, context switches, and the
 * finish-spread fairness figure. Each row reports the single repetition whose
 * throughput was the median, so its columns describe one coherent run.
 *
 * Its cpu/wall also saturates near producers+1 rather than 2.0, so these
 * numbers are NOT comparable with bench_ringbuf_vs_ringbuf_mutex.c's even
 * where the scenarios look alike. Failed reserves and pops yield after
 * YIELD_SPINS consecutive failures rather than spinning forever, applied
 * identically to all four rows.
 *
 * ONE HARNESS RULE THAT THE OTHER BENCHMARKS DO NOT NEED. With reservations
 * outstanding, a false pop never means "drained": the oldest slot may be
 * reserved but unsubmitted, which blocks delivery head-of-line in BOTH
 * variants. The consumer may only conclude the stream is over once every
 * producer has finished and every reservation has been submitted, which is
 * what the producers_left counter tracks -- a producer decrements it only
 * after its final submit.
 *
 * The consumer is pinned to one CPU and each producer to another, cycling over
 * the remaining CPUs; an unpinned mid-run migration shows up as a throughput
 * cliff unrelated to the code.
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
#include "ringbuf_zcopy.h"

#define REPS 5
#define TP_MESSAGES 2000000u

/* Upper bound on concurrent producers, sized for the oversubscribed
 * scenario rather than for the core count. */
#define MAX_PRODUCERS 16

/* Failed pops tolerated after the last producer signals done before the
 * consumer calls it empty. A single retry is not enough in either variant:
 * both return false on head-of-line waits and on contention, and breaking on
 * the first false would undercount received and break the
 * received + dropped == pushed check. */
#define DRAIN_SPINS 10000u

/* Consecutive failures before a retry loop yields instead of spinning. */
#define YIELD_SPINS 64u

/* Default fill cost: one word-wise pass over the payload, which is the
 * cheapest fill that still touches every byte. Scenario [4] varies it. */
#define DEFAULT_PASSES 1u

/* ---- Paced scenario parameters ----
 *
 * In the saturated scenarios every thread spins flat out, so cpu/wall pins
 * near its ceiling and CPU-per-message degenerates into a restatement of
 * throughput. The paced scenario fixes the offered rate below what any
 * variant can sustain and lets all sides idle, which is the only way the CPU
 * columns carry information the throughput column does not. */
#define PACE_MESSAGES 1000000u
#define PACE_BURST 100u
#define PACE_BURST_NS 100000u
#define PACE_SPINS 200u
#define PACE_BACKOFF_NS 50000u

/*
 * The four arrangements. VARIANT_MPSC_RSV and VARIANT_ZCOPY_RSV are the
 * headline pair -- same split operation, same single copy, differing only in
 * who synchronizes the producers. The two _PUSH rows are the controls that
 * copy twice.
 *
 * VARIANT_ZCOPY_RAW_PUSH calls ringbuf_zcopy_push() with no external lock at
 * all. It is valid ONLY in the single-threaded scenario, where it prices the
 * external mutex by difference; using it with more than one producer would be
 * undefined behavior, not a slow result.
 */
enum variant {
    VARIANT_MPSC_RSV,
    VARIANT_ZCOPY_RSV,
    VARIANT_MPSC_PUSH,
    VARIANT_ZCOPY_PUSH,
    VARIANT_ZCOPY_RAW_PUSH,
};

/**
 * variant_name - printable name of an arrangement
 * @v: which arrangement
 *
 * Return: a static string, never NULL.
 */
static const char *variant_name(enum variant v)
{
    switch (v) {
    case VARIANT_MPSC_RSV:
        return "ringbuf_mpsc rsv";
    case VARIANT_ZCOPY_RSV:
        return "ringbuf_zcopy+mu";
    case VARIANT_MPSC_PUSH:
        return "ringbuf_mpsc push";
    case VARIANT_ZCOPY_PUSH:
        return "ringbuf_zcopy+mu psh";
    default:
        return "ringbuf_zcopy raw";
    }
}

/**
 * variant_is_mpsc - does this arrangement use ringbuf_mpsc?
 * @v: which arrangement
 *
 * Return: true for the ringbuf_mpsc rows, false for the ringbuf_zcopy ones.
 */
static bool variant_is_mpsc(enum variant v)
{
    return v == VARIANT_MPSC_RSV || v == VARIANT_MPSC_PUSH;
}

/*
 * One scenario cell: everything that varies between measured rows. Passed by
 * pointer rather than as nine positional arguments, since several of them are
 * bools and sizes that would be easy to transpose at a call site.
 */
struct scenario {
    enum variant variant;
    bool overwrite;
    size_t slot_size;
    size_t capacity; /* lower bound; the ring rounds it up */
    unsigned producers;
    unsigned passes; /* fill cost: word-wise passes over the payload */
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
 * expressible on any host.
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
 * Falls back to a relaxed fence on non-x86, which at least keeps the compiler
 * from hoisting the load out of the spin loop.
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
 * backoff - pause after a failed reserve or pop, yielding once it persists
 * @spins: caller's consecutive-failure counter, updated in place
 *
 * Spins for the first YIELD_SPINS failures and yields after that. Reset
 * @spins to 0 on success.
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
 * Counts only on-CPU time, so a thread blocked on a futex accrues none of it
 * while descheduled. That is exactly the asymmetry being measured: the mutex
 * rows park their losers, ringbuf_mpsc's losers burn cycles.
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
 * Warns rather than failing: an unpinned run still produces numbers, they are
 * just noisier.
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
 * Used only by the paced scenario. Retries on EINTR so a signal cannot
 * silently shorten the pace interval.
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
 * Used to pick one whole repetition to report, so that every column of a
 * result row comes from the same run. Taking a per-column median instead
 * would mix runs and could print a CPU figure that never accompanied the
 * printed throughput.
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

/* ---- The fill, which is what the two arrangements overlap differently ---- */

/**
 * fill - write a payload into a slot at a controlled cost
 * @dst: slot_size writable bytes
 * @size: how many bytes @dst points at
 * @key: value the written pattern is derived from
 * @passes: how many read-modify-write passes to make over the payload
 *
 * The work being parallelized, and the axis scenario [4] sweeps. Writes
 * through a volatile pointer so the compiler cannot elide a pass or collapse
 * several into one, which would silently remove the thing being measured. The
 * first word carries @key unmodified so the consumer can check the message
 * arrived intact.
 *
 * Cost scales with @size * @passes. Modelled on a real producer assembling a
 * message -- a serialization pass, a checksum, a transform -- rather than on a
 * bare memcpy, because a fill cheap enough to be free is the one configuration
 * where neither variant's design matters.
 */
static void fill(void *dst, size_t size, uint64_t key, unsigned passes)
{
    volatile uint64_t *vp = dst;
    size_t words = size / sizeof(uint64_t);

    if (words == 0) {
        memcpy(dst, &key, size < sizeof(key) ? size : sizeof(key));
        return;
    }

    for (size_t i = 0; i < words; i++)
        vp[i] = key + i;

    for (unsigned p = 1; p < passes; p++)
        for (size_t i = 0; i < words; i++)
            vp[i] = vp[i] * 31u + key + p;

    vp[0] = key;
}

/* ---- The two queues behind one interface ----
 *
 * Exactly one of @mpsc and @zc is non-NULL. @push_lock is the caller-supplied
 * producer mutex that makes ringbuf_zcopy usable from N threads; it is
 * initialized only for the two ringbuf_zcopy+mu variants.
 *
 * _Alignas(64) on push_lock keeps it off the line holding the queue pointers,
 * which every producer reads on every claim. Without that the measurement
 * would include false sharing introduced by this harness.
 */
struct queue {
    enum variant variant;
    size_t slot_size;
    unsigned passes;
    struct ringbuf_mpsc *mpsc;
    struct ringbuf_zcopy *zc;
    _Alignas(64) pthread_mutex_t push_lock;
};

/**
 * q_init - create the queue a scenario calls for
 * @q: queue to initialize
 * @s: scenario describing which arrangement and geometry
 *
 * Return: true on success; false if allocation or mutex init failed, in which
 * case @q needs no teardown.
 */
static bool q_init(struct queue *q, const struct scenario *s)
{
    memset(q, 0, sizeof(*q));
    q->variant = s->variant;
    q->slot_size = s->slot_size;
    q->passes = s->passes ? s->passes : DEFAULT_PASSES;

    if (variant_is_mpsc(s->variant)) {
        q->mpsc = ringbuf_mpsc_create(s->slot_size, s->capacity, s->overwrite);
        return q->mpsc != NULL;
    }

    q->zc = ringbuf_zcopy_create(s->slot_size, s->capacity, s->overwrite);
    if (!q->zc)
        return false;
    if (s->variant != VARIANT_ZCOPY_RAW_PUSH &&
        pthread_mutex_init(&q->push_lock, NULL) != 0) {
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
    if (q->mpsc)
        ringbuf_mpsc_destroy(q->mpsc);
    if (q->zc) {
        if (q->variant != VARIANT_ZCOPY_RAW_PUSH)
            pthread_mutex_destroy(&q->push_lock);
        ringbuf_zcopy_destroy(q->zc);
    }
}

/**
 * q_store - store one message, by whichever arrangement this queue uses
 * @q: the queue
 * @key: value the payload pattern is derived from
 * @staging: scratch buffer of slot_size bytes, used only by the push rows
 *
 * The one function the whole comparison turns on, so the four cases are worth
 * reading side by side:
 *
 *   MPSC_RSV    claim with the ticket CAS, no lock at all, then fill the slot
 *               directly and submit. One copy, nothing serialized.
 *   ZCOPY_RSV   claim under the caller's mutex and release it IMMEDIATELY,
 *               then fill the slot and submit with the lock not held. One
 *               copy; only the claim is serialized, which is the arrangement
 *               ringbuf_zcopy.h prescribes.
 *   MPSC_PUSH   fill staging, then push -- which copies staging into the
 *               slot. Two touches of the payload, no lock.
 *   ZCOPY_PUSH  fill staging, then push under the mutex. Two touches, and the
 *               copy happens INSIDE the lock, which is what the split push
 *               exists to avoid.
 *
 * Return: true if stored; false if full, on head-of-line refusal, or on
 * transient contention. The caller retries -- with no lock held, which
 * matters: a refused reserve is usually waiting on a peer's submit.
 */
static bool q_store(struct queue *q, uint64_t key, void *staging)
{
    switch (q->variant) {
    case VARIANT_MPSC_RSV: {
        struct ringbuf_mpsc_rsv rsv;
        if (!ringbuf_mpsc_reserve(q->mpsc, &rsv))
            return false;
        fill(rsv.data, q->slot_size, key, q->passes);
        ringbuf_mpsc_submit(q->mpsc, &rsv);
        return true;
    }
    case VARIANT_ZCOPY_RSV: {
        struct ringbuf_rsv rsv;

        /* The lock covers the claim and nothing else. */
        pthread_mutex_lock(&q->push_lock);
        bool got = ringbuf_zcopy_reserve(q->zc, &rsv);
        pthread_mutex_unlock(&q->push_lock);

        if (!got)
            return false;

        /* Outside the lock: this is the work being parallelized, and also
         * why submit must not need the mutex. */
        fill(rsv.data, q->slot_size, key, q->passes);
        ringbuf_zcopy_submit(q->zc, &rsv);
        return true;
    }
    case VARIANT_MPSC_PUSH:
        fill(staging, q->slot_size, key, q->passes);
        return ringbuf_mpsc_push(q->mpsc, staging);
    case VARIANT_ZCOPY_RAW_PUSH:
        fill(staging, q->slot_size, key, q->passes);
        return ringbuf_zcopy_push(q->zc, staging);
    default: {
        fill(staging, q->slot_size, key, q->passes);
        pthread_mutex_lock(&q->push_lock);
        bool ok = ringbuf_zcopy_push(q->zc, staging);
        pthread_mutex_unlock(&q->push_lock);
        return ok;
    }
    }
}

/**
 * q_pop - copy one message out
 * @q: the queue
 * @data: destination for slot_size bytes
 *
 * Never takes push_lock: the consumer contends only with whatever the
 * implementation does internally, which is the asymmetry under test.
 *
 * Return: true if a message was copied out; false if empty, on a head-of-line
 * wait, or on transient contention.
 */
static bool q_pop(struct queue *q, void *data)
{
    if (variant_is_mpsc(q->variant))
        return ringbuf_mpsc_pop(q->mpsc, data);
    return ringbuf_zcopy_pop(q->zc, data);
}

/**
 * q_dropped - eviction count
 * @q: the queue
 *
 * Return: messages evicted before they were read; 0 in lossless mode.
 */
static size_t q_dropped(const struct queue *q)
{
    if (variant_is_mpsc(q->variant))
        return ringbuf_mpsc_dropped_count(q->mpsc);
    return ringbuf_zcopy_dropped_count(q->zc);
}

/* ---- Worker threads ----
 *
 * _Alignas(64) on the first member keeps each worker on its own cache line(s)
 * -- it raises the struct's alignment, which also pads sizeof up to a multiple
 * of 64, so an array of them stays line-separated. Every thread writes its own
 * instance on every iteration, so sharing a line would inject false sharing
 * into the thing being measured.
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
 * voluntary count is the interesting one: it is where blocking on the external
 * mutex's futex shows up, and it is the cost ringbuf_mpsc trades for spin
 * cycles.
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
 * Counts its own retries, CPU time, and wall time. The wall time feeds the
 * finish-spread figure: with N producers on one queue, how evenly the N shares
 * complete is a property of the synchronization.
 *
 * Decrements producers_left only after its last store has been submitted,
 * which is what lets the consumer distinguish a head-of-line wait from a
 * drained stream.
 *
 * Return: NULL always; results are reported through @arg.
 */
static void *producer_thread(void *arg)
{
    struct worker *w = arg;
    pin_to(producer_cpu(w->id));
    char *staging = calloc(1, w->slot_size);
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
        /* The key carries the producer id in the high byte so a consumer-side
         * check could tell the streams apart. */
        uint64_t key = seq | ((uint64_t)w->id << 56);

        /* Retry with NO lock held -- q_store releases the push mutex before
         * returning false, which is what keeps a peer's submit able to run. */
        while (!q_store(w->q, key, staging)) {
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
    free(staging);
    return NULL;
}

/**
 * consumer_thread - pin, then drain until every producer is done
 * @arg: struct worker
 *
 * Optionally does @consumer_work units of dummy dependent work per message, to
 * simulate a consumer that cannot keep up. That is what drives the buffer
 * permanently full and makes overwrite-mode eviction the common case.
 *
 * A false pop never means "drained" here: in both variants the oldest slot may
 * be reserved but unsubmitted, which blocks delivery head-of-line. So the
 * drain check requires producers_left == 0 -- every producer has submitted its
 * last message -- and then DRAIN_SPINS further failures.
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
            /* Volatile, so each iteration is a real load/store the optimizer
             * cannot fold away or hoist out. */
            for (unsigned i = 0; i < w->consumer_work; i++)
                w->sink += (w->sink >> 3) | 1u;
            w->received++;
            idle = 0;
            miss = 0;
            spins = 0;
            continue;
        }

        /* Retries are counted only while producers are still running, so the
         * metric stays a contention signal and does not absorb up to
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
     * mode msgs_per_sec counts STORES, and evicting is cheaper than
     * delivering -- so a variant can post a higher store rate purely by
     * dropping more. This is the metric that cannot be gamed that way. */
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
 * Return: the measurements for this single run; a zeroed result if the queue
 * could not be created.
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
 * Return: the one repetition whose throughput was the median, so that all of
 * its columns describe the same run.
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
 * bench_single - time alternating store/pop on one thread
 * @s: scenario to run; its producers and paced fields are ignored
 * @cpu_ns_per_op: out-param for CPU nanoseconds per store+pop pair
 *
 * Queue depth stays at 1, so there is no cross-core coherence traffic and no
 * contention at all: this isolates the uncontended instruction cost of each
 * arrangement. Running VARIANT_ZCOPY_RAW_PUSH alongside VARIANT_ZCOPY_PUSH is
 * what prices the external mutex on its own, separately from any contention
 * for it.
 *
 * Return: median messages/sec over REPS repetitions, where one message is a
 * full store+pop pair.
 */
static double bench_single(const struct scenario *s, double *cpu_ns_per_op)
{
    double rate[REPS];
    double cpu[REPS];
    char *staging = calloc(1, s->slot_size);
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
            q_store(&q, j, staging);
            q_pop(&q, out);
        }

        uint64_t c0 = thread_cpu_ns();
        uint64_t t0 = now_ns();
        for (unsigned n = 0; n < TP_MESSAGES; n++) {
            while (!q_store(&q, n, staging))
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

    free(staging);
    free(out);

    int m = median_index(rate, REPS);
    *cpu_ns_per_op = cpu[m];
    return rate[m];
}

/* ---- Output ---- */

/**
 * print_header - column headings for the N-producer tables
 *
 * store-Mm/s is stores completed per second; recv-Mm/s is messages actually
 * delivered. They differ only in overwrite mode, and that gap is the point.
 */
static void print_header(void)
{
    printf("  %-21s %9s %9s %9s %8s %8s %8s %9s %8s %7s\n", "impl",
           "store-Mm/s", "recv-Mm/s", "cpu-ns/rx", "cpu/wall", "prod-ns",
           "cons-ns", "stor-rt/k", "pop-rt/k", "ctxsw/k");
}

/**
 * print_row - one measured row, plus the second line it needs
 * @label: row label, normally the arrangement name
 * @r: measurements to print
 * @show_drop: include the eviction percentage (overwrite mode only)
 *
 * The CPU column is normalized per DELIVERED message, not per store, so it
 * cannot be improved by dropping more; prod-ns and cons-ns stay per store,
 * since that is the work those threads actually performed.
 */
static void print_row(const char *label, const struct result *r, bool show_drop)
{
    double cpu_per_rx =
        r->received ? r->cpu_ns_per_msg * r->msgs_per_sec / r->recv_per_sec :
                      0.0;

    printf("  %-21s %9.2f %9.2f %9.1f %8.2f %8.1f %8.1f %9.1f %8.1f %7.2f\n",
           label, r->msgs_per_sec / 1e6, r->recv_per_sec / 1e6, cpu_per_rx,
           r->cpu_per_wall, r->prod_cpu_ns_per_msg, r->cons_cpu_ns_per_msg,
           r->push_retries_per_k, r->pop_retries_per_k, r->ctxsw_per_k);

    printf("  %-21s  ", "");
    if (show_drop)
        printf("dropped %.2f%%, received %" PRIu64 ", ", r->drop_pct,
               r->received);
    printf("finish spread %.2fx%s\n", r->finish_spread,
           r->accounted ? "" : "  !! received + dropped != pushed");
}

/**
 * bench_quad - run all four arrangements for one cell and print four rows
 * @proto: scenario to run; its variant field is overwritten
 *
 * Every table in main() is this shape, so the loop lives here rather than
 * being repeated eight times. The order puts the two reserve rows first,
 * since rows 1 vs 2 are the headline comparison and the push rows are
 * controls.
 */
static void bench_quad(const struct scenario *proto)
{
    static const enum variant variants[] = { VARIANT_MPSC_RSV,
                                             VARIANT_ZCOPY_RSV,
                                             VARIANT_MPSC_PUSH,
                                             VARIANT_ZCOPY_PUSH };

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
 * Takes no arguments: the scenario set and its parameters are fixed at compile
 * time.
 *
 * Return: 0 always; this is a measurement, not a pass/fail check.
 */
int main(void)
{
    static const unsigned prod_counts[] = { 1, 2, 4, 7 };
    static const size_t caps[] = { 16, 256, 4096, 65536 };
    static const size_t sizes[] = { 16, 64, 256, 1024 };
    static const unsigned pass_counts[] = { 1, 2, 8, 32 };

    g_ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (g_ncpu < 1)
        g_ncpu = 1;
    pin_to(CONSUMER_CPU);

    printf("ringbuf_mpsc vs ringbuf_zcopy, multi-producer, both split-push\n");
    printf("%u messages per run split across the producers, median of %d"
           " runs\n",
           TP_MESSAGES, REPS);
    printf("consumer pinned to cpu %d, producers to cpus 1..%d (cycling);"
           " %d cpus online\n",
           CONSUMER_CPU, g_ncpu - 1, g_ncpu);
    printf("ringbuf_zcopy needs the CALLER to serialize reserves, so its rows"
           " hold an external\n  pthread_mutex_t across the CLAIM ONLY --"
           " lock / reserve / unlock -- and fill and\n  submit with it"
           " released. ringbuf_mpsc needs no such lock: its write index is\n"
           "  multi-writer behind a ticket CAS.\n");
    printf("Four rows per table. The first two are the headline: same split"
           " operation, one copy\n  each, differing only in who synchronizes"
           " the producers. The two `push` rows are\n  controls that assemble"
           " in a staging buffer and copy again, which is what the\n  split"
           " push exists to avoid.\n");
    printf("cpu/wall saturates near producers+1 here, not 2.0, so these"
           " numbers are not\ncomparable with"
           " bench_ringbuf_vs_ringbuf_mutex.\n");
    printf("In overwrite mode compare recv-Mm/s, not store-Mm/s: evicting is"
           " cheaper than\ndelivering, so a variant can post a higher store"
           " rate just by dropping more.\n");
    if (prod_counts[sizeof prod_counts / sizeof *prod_counts - 1] >
        (unsigned)(g_ncpu - 1))
        printf("NOTE: fewer cpus than the widest scenario asks for; the wide"
               " rows share cores\n  and are oversubscribed like scenario"
               " [7].\n");

    printf("\n[1] Single thread, alternating store/pop (depth 1), 64B slots,"
           " cap 1024\n");
    printf("    Uncontended cost, and the price of the external mutex on its"
           " own: the raw row\n    takes no push lock (legal only because"
           " there is one thread here), so the gap to\n    +mu psh is one"
           " uncontended lock/unlock pair. With no contention and no fill\n"
           "    overlap to win, this is where the ticket CAS's standing cost"
           " shows up naked.\n");
    printf("  %-21s %10s %8s %10s\n", "impl", "mode", "Mmsg/s", "cpu-ns/op");
    for (unsigned m = 0; m < 2; m++) {
        static const enum variant single[] = {
            VARIANT_MPSC_RSV, VARIANT_ZCOPY_RSV, VARIANT_MPSC_PUSH,
            VARIANT_ZCOPY_PUSH, VARIANT_ZCOPY_RAW_PUSH
        };
        for (unsigned i = 0; i < sizeof single / sizeof *single; i++) {
            struct scenario s = { .variant = single[i],
                                  .overwrite = m == 1,
                                  .slot_size = 64,
                                  .capacity = 1024,
                                  .producers = 1,
                                  .passes = DEFAULT_PASSES };
            double cpu_op = 0;
            double rate = bench_single(&s, &cpu_op);
            printf("  %-21s %10s %8.2f %10.2f\n", variant_name(single[i]),
                   s.overwrite ? "overwrite" : "lossless", rate / 1e6, cpu_op);
        }
    }

    printf("\n[2] Lossless, 64B slots, cap 4096, by producer count\n");
    printf("    The headline sweep. One producer is the control: there the"
           " external mutex is\n    uncontended and the ticket CAS"
           " uncontested, so any gap is instruction cost.\n    Past that, this"
           " is a lock-free claim against a serialized one.\n");
    for (unsigned p = 0; p < sizeof prod_counts / sizeof *prod_counts; p++) {
        printf("  producers=%u\n", prod_counts[p]);
        struct scenario s = { .overwrite = false,
                              .slot_size = 64,
                              .capacity = 4096,
                              .producers = prod_counts[p],
                              .passes = DEFAULT_PASSES };
        bench_quad(&s);
    }

    printf("\n[3] Overwrite, 64B slots, cap 4096, by producer count\n");
    printf("    Same sweep with eviction in play. ringbuf_mpsc's evicting"
           " producers take a slot\n    lock and can fail; ringbuf_zcopy's"
           " take its internal mutex on top of the\n    caller's. Both refuse"
           " a slot whose reservation is unsubmitted, so the reserve\n    rows"
           " can fail here where the push rows cannot.\n");
    for (unsigned p = 0; p < sizeof prod_counts / sizeof *prod_counts; p++) {
        printf("  producers=%u\n", prod_counts[p]);
        struct scenario s = { .overwrite = true,
                              .slot_size = 64,
                              .capacity = 4096,
                              .producers = prod_counts[p],
                              .passes = DEFAULT_PASSES };
        bench_quad(&s);
    }

    printf("\n[4] Lossless, 4 producers, 256B slots, cap 4096, BY FILL COST\n");
    printf("    The table this benchmark exists for. Filling is the work the"
           " two arrangements\n    overlap differently: the mutex covers only"
           " the claim, so N fills proceed\n    concurrently, while the push"
           " rows serialize the copy inside whatever lock they\n    hold. At"
           " passes=1 the fill is nearly free and the mutex is pure overhead;"
           " as it\n    grows, overlapping it is the whole game. If"
           " ringbuf_zcopy's design ever pays\n    off, it is here -- and the"
           " crossover point is the result.\n");
    for (unsigned z = 0; z < sizeof pass_counts / sizeof *pass_counts; z++) {
        printf("  passes=%u\n", pass_counts[z]);
        struct scenario s = { .overwrite = false,
                              .slot_size = 256,
                              .capacity = 4096,
                              .producers = 4,
                              .passes = pass_counts[z] };
        bench_quad(&s);
    }

    printf("\n[5] Lossless, 4 producers, cap 4096, by slot size\n");
    printf("    The other way to make the fill expensive -- more bytes rather"
           " than more passes\n    over them. Checks that the synchronization"
           " gap does not simply vanish under\n    memcpy cost as payloads"
           " grow.\n");
    for (unsigned z = 0; z < sizeof sizes / sizeof *sizes; z++) {
        printf("  slot=%zuB\n", sizes[z]);
        struct scenario s = { .overwrite = false,
                              .slot_size = sizes[z],
                              .capacity = 4096,
                              .producers = 4,
                              .passes = DEFAULT_PASSES };
        bench_quad(&s);
    }

    printf("\n[6] Overwrite, 4 producers, 64B slots, by capacity\n");
    printf("    Small capacities fill constantly, so the eviction paths are"
           " hit hard and\n    head-of-line stalls on an unsubmitted"
           " reservation get much more likely; large\n    ones rarely fill at"
           " all and isolate the claim cost.\n");
    for (unsigned c = 0; c < sizeof caps / sizeof *caps; c++) {
        printf("  cap=%zu\n", caps[c]);
        struct scenario s = { .overwrite = true,
                              .slot_size = 64,
                              .capacity = caps[c],
                              .producers = 4,
                              .passes = DEFAULT_PASSES };
        bench_quad(&s);
    }

    printf("\n[7] Oversubscribed: %u producers on %d producer cpus\n",
           MAX_PRODUCERS, g_ncpu - 1);
    printf("    Both variants have a head-of-line stall to expose here, and"
           " they are not the\n    same one. A descheduled ringbuf_mpsc"
           " producer holding an unsubmitted ticket\n    blocks delivery;"
           " a descheduled ringbuf_zcopy producer does too, AND its peers\n"
           "    cannot even claim a slot while it holds the push mutex's"
           " queue. Both rows yield\n    after %u failed attempts.\n",
           YIELD_SPINS);
    for (unsigned m = 0; m < 2; m++) {
        printf("  %s, cap 4096\n", m ? "overwrite" : "lossless");
        struct scenario s = { .overwrite = m == 1,
                              .slot_size = 64,
                              .capacity = 4096,
                              .producers = MAX_PRODUCERS,
                              .passes = DEFAULT_PASSES };
        bench_quad(&s);
    }

    printf("\n[8] Paced below saturation, 4 producers, 256B slots (%u msgs,"
           " bursts of %u per %u us each)\n",
           PACE_MESSAGES, PACE_BURST, PACE_BURST_NS / 1000);
    printf("    Throughput here is set by the pacing, not the queue, so ignore"
           " Mm/s and read the\n    CPU columns: this is the only scenario"
           " where no thread spins flat out, so\n    cpu/wall is free to fall"
           " well below its ceiling and CPU per message becomes a\n    real"
           " measurement rather than a restatement of wall time.\n");
    for (unsigned m = 0; m < 2; m++) {
        printf("  %s, cap 4096\n", m ? "overwrite" : "lossless");
        struct scenario s = { .overwrite = m == 1,
                              .slot_size = 256,
                              .capacity = 4096,
                              .producers = 4,
                              .passes = DEFAULT_PASSES,
                              .paced = true };
        bench_quad(&s);
    }

    return 0;
}
