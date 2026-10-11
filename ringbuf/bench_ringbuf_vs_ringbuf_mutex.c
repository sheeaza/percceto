/*
 * Head-to-head benchmark: ringbuf (lock-free) vs ringbuf_mutex.
 *
 * bench_ringbuf.c measures ringbuf against a toy mutex-protected ring to
 * price the lock in the abstract. This file instead measures the two real
 * implementations against each other, and reports CPU consumption next to
 * throughput -- because the two designs spend CPU in different places and
 * a messages/sec column alone hides the difference:
 *
 *   - ringbuf resolves eviction contention by failing the call. The caller
 *     retries, so the cost lands as spin cycles on the caller's core: CPU
 *     burned, no context switches.
 *   - ringbuf_mutex resolves it by blocking on a futex. The cost lands as
 *     voluntary context switches and descheduled time: fewer cycles
 *     burned, more scheduler involvement, and latency outliers.
 *
 * So every row reports wall throughput, CPU-nanoseconds per message summed
 * over both threads, CPU-to-wall ratio, retry counts, and context switches.
 * Each metric is reported from the single repetition whose throughput was
 * the median, so the columns of a row describe one coherent run rather than
 * medians taken independently per column.
 *
 * Both threads are pinned to fixed, distinct CPUs: an unpinned mid-run
 * migration shows up as a throughput cliff unrelated to the code.
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

#include "ringbuf.h"
#include "ringbuf_mutex.h"

#define PRODUCER_CPU 2
#define CONSUMER_CPU 4

#define REPS 5
#define TP_MESSAGES 2000000u

/* Failed pops tolerated after the producer signals done before the consumer
 * calls it empty. A single retry is not enough: in the lock-free variant's
 * overwrite mode a false return can mean transient contention rather than
 * empty, and breaking on it would undercount received and break the
 * received + dropped == pushed check. */
#define DRAIN_SPINS 10000u

/* ---- Paced scenario parameters (scenario [6]) ----
 *
 * In the saturated scenarios both threads spin flat out, so cpu/wall pins
 * at ~2.0 and CPU-per-message degenerates into a restatement of
 * throughput. The paced scenario fixes the offered rate below what either
 * variant can sustain and lets both sides idle, which is the only way the
 * CPU columns carry information the throughput column does not.
 *
 * The producer sends PACE_BURST messages every PACE_BURST_NS, sleeping in
 * between, rather than pacing each message individually -- a per-message
 * sleep at this rate would be dominated by the sleep syscall itself. The
 * consumer backs off into a sleep after PACE_SPINS failed pops. Both
 * behaviors are identical for the two variants, so their cost cancels in
 * the comparison. */
#define PACE_MESSAGES 1000000u
#define PACE_BURST 100u
#define PACE_BURST_NS 100000u
#define PACE_SPINS 200u
#define PACE_BACKOFF_NS 50000u

enum variant { VARIANT_LOCKFREE, VARIANT_MUTEX };

/**
 * variant_name - printable name of an implementation
 * @v: which implementation
 *
 * Return: a static string, never NULL.
 */
static const char *variant_name(enum variant v)
{
    return v == VARIANT_MUTEX ? "ringbuf_mutex" : "ringbuf";
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
 * it while descheduled. That is exactly the asymmetry being measured.
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

/* ---- Dispatch over the two implementations ---- */

/**
 * q_create - allocate a buffer of the given implementation
 * @v: which implementation
 * @slot_size: size of each slot in bytes
 * @capacity: lower bound on the slot count
 * @overwrite: create in overwrite-on-full mode
 *
 * Return: opaque buffer pointer, or NULL on failure.
 */
static void *q_create(enum variant v, size_t slot_size, size_t capacity,
                      bool overwrite)
{
    if (v == VARIANT_MUTEX)
        return ringbuf_mutex_create(slot_size, capacity, overwrite);
    return ringbuf_create(slot_size, capacity, overwrite);
}

/**
 * q_destroy - free a buffer from q_create()
 * @v: which implementation
 * @q: the buffer, or NULL
 */
static void q_destroy(enum variant v, void *q)
{
    if (v == VARIANT_MUTEX)
        ringbuf_mutex_destroy(q);
    else
        ringbuf_destroy(q);
}

/**
 * q_push - copy one message in
 * @v: which implementation
 * @q: the buffer
 * @data: slot_size bytes to copy in
 *
 * Return: true if stored; false if full, or -- lock-free overwrite mode
 * only -- on transient contention.
 */
static bool q_push(enum variant v, void *q, const void *data)
{
    if (v == VARIANT_MUTEX)
        return ringbuf_mutex_push(q, data);
    return ringbuf_push(q, data);
}

/**
 * q_pop - copy one message out
 * @v: which implementation
 * @q: the buffer
 * @data: destination for slot_size bytes
 *
 * Return: true if a message was copied out; false if empty, or -- lock-free
 * overwrite mode only -- on transient contention.
 */
static bool q_pop(enum variant v, void *q, void *data)
{
    if (v == VARIANT_MUTEX)
        return ringbuf_mutex_pop(q, data);
    return ringbuf_pop(q, data);
}

/**
 * q_dropped - eviction count
 * @v: which implementation
 * @q: the buffer
 *
 * Return: messages evicted before they were read; 0 in lossless mode.
 */
static size_t q_dropped(enum variant v, const void *q)
{
    if (v == VARIANT_MUTEX)
        return ringbuf_mutex_dropped_count(q);
    return ringbuf_dropped_count(q);
}

/* ---- Worker threads ---- */

/*
 * _Alignas(64) on the first member keeps the producer's and consumer's
 * instances on separate cache lines -- it raises the struct's own alignment,
 * which also pads sizeof up to a multiple of 64. (It has to sit on a member:
 * C11 does not accept an alignment specifier on a struct tag.) Both threads
 * write their own copy every iteration, so sharing a line would inject false
 * sharing into the thing being measured. The per-thread `sink` lives in here
 * for the same reason -- a single global sink written from both cores is
 * itself a coherence benchmark.
 */
struct worker {
    _Alignas(64) void *q;
    enum variant variant;
    size_t slot_size;
    uint64_t count; /* producer: messages to send */
    unsigned consumer_work; /* consumer: dummy work units per message */
    bool paced; /* rate-limit the producer, back the consumer off */
    atomic_bool *done;

    /* Written by the owning thread only. */
    volatile uint64_t sink;
    uint64_t retries;
    uint64_t received;
    uint64_t cpu_ns;
    uint64_t vol_ctxsw;
    uint64_t invol_ctxsw;
};

/**
 * record_ctxsw - sample this thread's context-switch counters
 * @w: worker to store the counts into
 *
 * RUSAGE_THREAD is Linux-specific, hence the _GNU_SOURCE at the top. The
 * voluntary count is the interesting one: it is where blocking on the
 * mutex variant's futex shows up.
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
 * producer_thread - pin, then push count sequenced messages
 * @arg: struct worker
 *
 * Counts its own spin retries and CPU time. The retry count is the
 * lock-free variant's hidden cost: in overwrite mode a full buffer whose
 * oldest slot is being read right now fails the push, and the caller pays
 * for the retry.
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
    pin_to(PRODUCER_CPU);
    char *msg = calloc(1, w->slot_size);
    uint64_t next_burst = now_ns();

    uint64_t cpu0 = thread_cpu_ns();
    for (uint64_t seq = 0; seq < w->count; seq++) {
        if (w->paced && seq % PACE_BURST == 0) {
            next_burst += PACE_BURST_NS;
            uint64_t t = now_ns();
            if (next_burst > t)
                sleep_ns(next_burst - t);
        }
        memcpy(msg, &seq, sizeof(seq));
        while (!q_push(w->variant, w->q, msg)) {
            w->retries++;
            cpu_relax();
        }
    }
    atomic_store_explicit(w->done, true, memory_order_release);
    w->cpu_ns = thread_cpu_ns() - cpu0;

    record_ctxsw(w);
    free(msg);
    return NULL;
}

/**
 * consumer_thread - pin, then drain until the producer is done
 * @arg: struct worker
 *
 * Optionally does @consumer_work units of dummy dependent work per message,
 * to simulate a consumer that cannot keep up. That is what drives the
 * buffer permanently full and makes overwrite-mode eviction the common case
 * rather than a rarity.
 *
 * When @paced, backs off into a sleep after PACE_SPINS consecutive failed
 * pops instead of spinning indefinitely, so an idle consumer costs CPU time
 * rather than burning it.
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

    uint64_t cpu0 = thread_cpu_ns();
    for (;;) {
        if (q_pop(w->variant, w->q, out)) {
            uint64_t seq;
            memcpy(&seq, out, sizeof(seq));
            w->sink += seq;
            /* Volatile, so each iteration is a real load/store the
             * optimizer cannot fold away or hoist out. */
            for (unsigned i = 0; i < w->consumer_work; i++)
                w->sink += (w->sink >> 3) | 1u;
            w->received++;
            idle = 0;
            miss = 0;
            continue;
        }

        /* Retries are counted only while the producer is still running, so
         * the metric stays a contention signal and does not absorb up to
         * DRAIN_SPINS of end-of-run polling. */
        if (!atomic_load_explicit(w->done, memory_order_acquire)) {
            w->retries++;
            if (w->paced && ++miss >= PACE_SPINS) {
                miss = 0;
                sleep_ns(PACE_BACKOFF_NS);
            } else {
                cpu_relax();
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
    double cpu_ns_per_msg; /* both threads summed */
    double cpu_per_wall; /* 2.0 means both cores pegged */
    double prod_cpu_ns_per_msg;
    double cons_cpu_ns_per_msg;
    double push_retries_per_k;
    double pop_retries_per_k;
    double ctxsw_per_k; /* voluntary + involuntary, both threads */
    double drop_pct;
    uint64_t received;
    uint64_t dropped;
    bool accounted; /* received + dropped == pushed */
};

/**
 * run_spsc - one two-thread transfer, measured
 * @v: which implementation
 * @overwrite: create the buffer in overwrite mode
 * @slot_size: size of each slot in bytes; must hold a uint64_t
 * @capacity: lower bound on the slot count
 * @consumer_work: dummy work units the consumer does per message
 * @paced: rate-limit the producer and back the consumer off, so neither
 *	thread saturates its core
 *
 * Return: the measurements for this single run.
 */
static struct result run_spsc(enum variant v, bool overwrite, size_t slot_size,
                              size_t capacity, unsigned consumer_work,
                              bool paced)
{
    void *q = q_create(v, slot_size, capacity, overwrite);
    atomic_bool done = false;
    uint64_t total = paced ? PACE_MESSAGES : TP_MESSAGES;

    static struct worker prod, cons;
    memset(&prod, 0, sizeof(prod));
    memset(&cons, 0, sizeof(cons));

    prod.q = cons.q = q;
    prod.variant = cons.variant = v;
    prod.slot_size = cons.slot_size = slot_size;
    prod.done = cons.done = &done;
    prod.paced = cons.paced = paced;
    prod.count = total;
    cons.consumer_work = consumer_work;

    pthread_t pt, ct;
    uint64_t t0 = now_ns();
    pthread_create(&pt, NULL, producer_thread, &prod);
    pthread_create(&ct, NULL, consumer_thread, &cons);
    pthread_join(pt, NULL);
    pthread_join(ct, NULL);
    uint64_t t1 = now_ns();

    uint64_t wall = t1 - t0;
    uint64_t cpu = prod.cpu_ns + cons.cpu_ns;
    uint64_t dropped = q_dropped(v, q);
    double msgs = (double)total;
    double k = msgs / 1000.0;

    struct result r = {
        .msgs_per_sec = msgs * 1e9 / (double)wall,
        .recv_per_sec = (double)cons.received * 1e9 / (double)wall,
        .cpu_ns_per_msg = (double)cpu / msgs,
        .cpu_per_wall = (double)cpu / (double)wall,
        .prod_cpu_ns_per_msg = (double)prod.cpu_ns / msgs,
        .cons_cpu_ns_per_msg = (double)cons.cpu_ns / msgs,
        .push_retries_per_k = (double)prod.retries / k,
        .pop_retries_per_k = (double)cons.retries / k,
        .ctxsw_per_k = (double)(prod.vol_ctxsw + prod.invol_ctxsw +
                                cons.vol_ctxsw + cons.invol_ctxsw) /
                       k,
        .drop_pct = 100.0 * (double)dropped / msgs,
        .received = cons.received,
        .dropped = dropped,
        .accounted = cons.received + dropped == total,
    };

    q_destroy(v, q);
    return r;
}

/**
 * bench_spsc - run REPS transfers and keep the median-throughput one
 * @v: which implementation
 * @overwrite: create the buffer in overwrite mode
 * @slot_size: size of each slot in bytes
 * @capacity: lower bound on the slot count
 * @consumer_work: dummy work units the consumer does per message
 * @paced: rate-limit both sides rather than letting them saturate
 *
 * Return: the one repetition whose throughput was the median, so that all
 * of its columns describe the same run.
 */
static struct result bench_spsc(enum variant v, bool overwrite,
                                size_t slot_size, size_t capacity,
                                unsigned consumer_work, bool paced)
{
    struct result all[REPS];
    double rate[REPS];

    for (int r = 0; r < REPS; r++) {
        all[r] =
            run_spsc(v, overwrite, slot_size, capacity, consumer_work, paced);
        rate[r] = all[r].msgs_per_sec;
    }
    return all[median_index(rate, REPS)];
}

/**
 * bench_single - time alternating push/pop on one thread
 * @v: which implementation
 * @overwrite: create the buffer in overwrite mode
 * @slot_size: size of each slot in bytes
 * @capacity: lower bound on the slot count
 * @cpu_ns_per_op: out-param for CPU nanoseconds per push+pop pair
 *
 * Queue depth stays at 1, so there is no cross-core coherence traffic and
 * no contention at all: this isolates the uncontended instruction cost of
 * each implementation's synchronization. For overwrite mode that is
 * ringbuf's per-slot spinlock plus read_idx CAS against ringbuf_mutex's
 * uncontended lock/unlock pair -- which is never reached here, since the
 * buffer is never full, so both variants run their unlocked fast path.
 *
 * Return: median messages/sec over REPS repetitions.
 */
static double bench_single(enum variant v, bool overwrite, size_t slot_size,
                           size_t capacity, double *cpu_ns_per_op)
{
    double rate[REPS];
    double cpu[REPS];
    char *msg = calloc(1, slot_size);
    char *out = calloc(1, slot_size);

    for (int r = 0; r < REPS; r++) {
        void *q = q_create(v, slot_size, capacity, overwrite);

        /* Warm the slots so the timed loop is not measuring page faults. */
        for (size_t i = 0; i < capacity; i++) {
            q_push(v, q, msg);
            q_pop(v, q, out);
        }

        uint64_t c0 = thread_cpu_ns();
        uint64_t t0 = now_ns();
        for (unsigned i = 0; i < TP_MESSAGES; i++) {
            memcpy(msg, &i, sizeof(i));
            while (!q_push(v, q, msg))
                cpu_relax();
            while (!q_pop(v, q, out))
                cpu_relax();
        }
        uint64_t t1 = now_ns();
        uint64_t c1 = thread_cpu_ns();

        rate[r] = (double)TP_MESSAGES * 1e9 / (double)(t1 - t0);
        cpu[r] = (double)(c1 - c0) / (double)TP_MESSAGES;
        q_destroy(v, q);
    }

    free(msg);
    free(out);

    int m = median_index(rate, REPS);
    *cpu_ns_per_op = cpu[m];
    return rate[m];
}

/* ---- Output ---- */

/**
 * print_spsc_header - column headings for the two-thread tables
 *
 * push-Mm/s is pushes completed per second; recv-Mm/s is messages actually
 * delivered. They differ only in overwrite mode, and that gap is the point:
 * see struct result's recv_per_sec.
 */
static void print_spsc_header(void)
{
    printf("  %-16s %9s %9s %9s %8s %8s %8s %9s %8s %7s\n", "impl", "push-Mm/s",
           "recv-Mm/s", "cpu-ns/rx", "cpu/wall", "prod-ns", "cons-ns",
           "push-rt/k", "pop-rt/k", "ctxsw/k");
}

/**
 * print_spsc_row - one measured row, plus any warning lines it needs
 * @label: row label, normally the implementation name
 * @r: measurements to print
 * @show_drop: append the eviction percentage line (overwrite mode only)
 *
 * The CPU column is normalized per DELIVERED message, not per push, so it
 * cannot be improved by dropping more; prod-ns and cons-ns stay per push,
 * since that is the work each thread actually performed.
 */
static void print_spsc_row(const char *label, const struct result *r,
                           bool show_drop)
{
    double cpu_per_rx =
        r->received ? r->cpu_ns_per_msg * r->msgs_per_sec / r->recv_per_sec :
                      0.0;

    printf("  %-16s %9.2f %9.2f %9.1f %8.2f %8.1f %8.1f %9.1f %8.1f %7.2f\n",
           label, r->msgs_per_sec / 1e6, r->recv_per_sec / 1e6, cpu_per_rx,
           r->cpu_per_wall, r->prod_cpu_ns_per_msg, r->cons_cpu_ns_per_msg,
           r->push_retries_per_k, r->pop_retries_per_k, r->ctxsw_per_k);

    if (show_drop)
        printf("  %-16s   dropped %.2f%%, received %" PRIu64 "%s\n", "",
               r->drop_pct, r->received,
               r->accounted ? "" : "  !! received + dropped != pushed");
    else if (!r->accounted)
        printf("  %-16s   !! received %" PRIu64 " of %u\n", "", r->received,
               TP_MESSAGES);
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
    static const enum variant variants[] = { VARIANT_LOCKFREE, VARIANT_MUTEX };
    static const size_t caps[] = { 16, 256, 4096, 65536 };
    static const size_t sizes[] = { 16, 64, 256, 1024 };

    pin_to(PRODUCER_CPU);

    printf("ringbuf vs ringbuf_mutex -- throughput and CPU consumption\n");
    printf("%u messages per run, median of %d runs, producer cpu %d ->"
           " consumer cpu %d\n",
           TP_MESSAGES, REPS, PRODUCER_CPU, CONSUMER_CPU);
    printf("Scenarios [2]-[5] saturate both threads, so cpu/wall pins near"
           " 2.0 there and\ncpu-ns/rx is ~2x the wall cost per message rather"
           " than an independent figure.\nScenario [6] paces the load below"
           " saturation, which is where the CPU columns\nsay something the"
           " throughput column does not.\n");
    printf("In overwrite mode compare recv-Mm/s, not push-Mm/s: evicting is"
           " cheaper than\ndelivering, so a variant can post a higher push"
           " rate just by dropping more.\n\n");

    printf("[1] Single thread, alternating push/pop (depth 1), 64B slots,"
           " cap 1024\n");
    printf("    Uncontended cost. Neither variant ever fills, so both run"
           " their unlocked\n    fast path and any gap here is not"
           " synchronization.\n");
    printf("  %-16s %10s %8s %10s\n", "impl", "mode", "Mmsg/s", "cpu-ns/op");
    for (unsigned m = 0; m < 2; m++) {
        bool overwrite = m == 1;
        for (unsigned i = 0; i < sizeof variants / sizeof *variants; i++) {
            double cpu_op = 0;
            double r = bench_single(variants[i], overwrite, 64, 1024, &cpu_op);
            printf("  %-16s %10s %8.2f %10.2f\n", variant_name(variants[i]),
                   overwrite ? "overwrite" : "lossless", r / 1e6, cpu_op);
        }
    }

    printf("\n[2] SPSC lossless, 64B slots, by capacity\n");
    printf("    Expected parity: in lossless mode read_idx has one writer,"
           " so neither\n    variant takes a lock and the code paths are"
           " equivalent.\n");
    for (unsigned c = 0; c < sizeof caps / sizeof *caps; c++) {
        printf("  cap=%zu\n", caps[c]);
        print_spsc_header();
        for (unsigned i = 0; i < sizeof variants / sizeof *variants; i++) {
            struct result r =
                bench_spsc(variants[i], false, 64, caps[c], 0, false);
            print_spsc_row(variant_name(variants[i]), &r, false);
        }
    }

    printf("\n[3] SPSC overwrite, 64B slots, by capacity\n");
    printf("    The real comparison. Small capacities fill often, so the"
           " eviction paths --\n    spinlock+CAS vs mutex -- are hit"
           " hard; large ones rarely fill at all.\n");
    for (unsigned c = 0; c < sizeof caps / sizeof *caps; c++) {
        printf("  cap=%zu\n", caps[c]);
        print_spsc_header();
        for (unsigned i = 0; i < sizeof variants / sizeof *variants; i++) {
            struct result r =
                bench_spsc(variants[i], true, 64, caps[c], 0, false);
            print_spsc_row(variant_name(variants[i]), &r, true);
        }
    }

    printf("\n[4] SPSC overwrite, cap 4096, by slot size\n");
    printf("    Checks that the synchronization gap does not simply vanish"
           " under memcpy\n    cost as payloads grow.\n");
    for (unsigned s = 0; s < sizeof sizes / sizeof *sizes; s++) {
        printf("  slot=%zuB\n", sizes[s]);
        print_spsc_header();
        for (unsigned i = 0; i < sizeof variants / sizeof *variants; i++) {
            struct result r =
                bench_spsc(variants[i], true, sizes[s], 4096, 0, false);
            print_spsc_row(variant_name(variants[i]), &r, true);
        }
    }

    printf("\n[5] SPSC overwrite, slow consumer (200 work units/msg)\n");
    printf("    Worst case for eviction: the consumer cannot keep up, so"
           " the buffer is\n    permanently full and nearly every push"
           " evicts. Consumer work inflates\n    cons-ns equally for both,"
           " so compare the two rows, not across tables.\n");
    printf("    Both variants drop ~90%% here, so push-Mm/s mostly measures"
           " how fast each\n    one can discard; recv-Mm/s is the column"
           " that reflects useful work.\n");
    for (unsigned c = 0; c < sizeof caps / sizeof *caps; c++) {
        if (caps[c] != 16 && caps[c] != 4096)
            continue;
        printf("  cap=%zu\n", caps[c]);
        print_spsc_header();
        for (unsigned i = 0; i < sizeof variants / sizeof *variants; i++) {
            struct result r =
                bench_spsc(variants[i], true, 64, caps[c], 200, false);
            print_spsc_row(variant_name(variants[i]), &r, true);
        }
    }

    printf("\n[6] SPSC paced below saturation, 64B slots (%u msgs, bursts of"
           " %u per %u us)\n",
           PACE_MESSAGES, PACE_BURST, PACE_BURST_NS / 1000);
    printf("    Throughput here is set by the pacing, not the buffer, so"
           " ignore Mmsg/s and\n    read the CPU columns: this is the only"
           " scenario where neither thread spins\n    flat out, so cpu/wall"
           " is free to drop below 2.0 and CPU cost per message\n    becomes"
           " a real measurement rather than 2x the wall time.\n");
    for (unsigned m = 0; m < 2; m++) {
        bool overwrite = m == 1;
        printf("  %s, cap 4096\n", overwrite ? "overwrite" : "lossless");
        print_spsc_header();
        for (unsigned i = 0; i < sizeof variants / sizeof *variants; i++) {
            struct result r =
                bench_spsc(variants[i], overwrite, 64, 4096, 0, true);
            print_spsc_row(variant_name(variants[i]), &r, overwrite);
        }
    }

    return 0;
}
