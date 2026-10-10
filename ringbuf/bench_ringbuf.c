/*
 * Benchmarks for the SPSC ring buffer.
 *
 * Reports the median of several repetitions rather than a single run, and
 * pins the producer and consumer to fixed, distinct CPUs -- without pinning
 * the scheduler migrating one of them mid-run shows up as a throughput
 * cliff that has nothing to do with the code under test.
 *
 * A mutex-protected ring with identical non-blocking semantics (lock,
 * check, copy, unlock; return false when full/empty) runs alongside as a
 * baseline. It is deliberately NOT a blocking queue with condvars: the
 * point is to isolate the cost of the lock itself, not to also measure
 * futex wakeup latency.
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
#include <time.h>

#include "ringbuf.h"

#define PRODUCER_CPU 2
#define CONSUMER_CPU 4

#define REPS 5
#define TP_MESSAGES 2000000u
#define LAT_MESSAGES 200000u

/* Gap the latency test leaves between sends. Keeps the queue near-empty so
 * the measurement is handoff latency and not queueing delay. */
#define LAT_GAP_NS 2000

/* Consumed by the consumer threads so the compiler cannot discard the
 * loads that make up the work being measured. */
static volatile uint64_t sink;

static void cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    atomic_thread_fence(memory_order_relaxed);
#endif
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static void pin_to(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
        fprintf(stderr, "warning: could not pin to cpu %d\n", cpu);
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static int cmp_dbl(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;
    return (x > y) - (x < y);
}

static double median_dbl(double *v, int n)
{
    qsort(v, (size_t)n, sizeof(*v), cmp_dbl);
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

/* ---- Mutex baseline, same non-blocking contract as ringbuf ---- */

struct mq {
    pthread_mutex_t lock;
    size_t capacity;
    size_t mask;
    size_t slot_size;
    size_t head; /* next to write */
    size_t tail; /* next to read */
    char *buf;
};

static struct mq *mq_create(size_t slot_size, size_t capacity)
{
    struct mq *q = calloc(1, sizeof(*q));
    if (!q)
        return NULL;
    q->buf = malloc(slot_size * capacity);
    if (!q->buf) {
        free(q);
        return NULL;
    }
    pthread_mutex_init(&q->lock, NULL);
    q->capacity = capacity;
    q->mask = capacity - 1;
    q->slot_size = slot_size;
    return q;
}

static void mq_destroy(struct mq *q)
{
    if (!q)
        return;
    pthread_mutex_destroy(&q->lock);
    free(q->buf);
    free(q);
}

static bool mq_push(struct mq *q, const void *data)
{
    pthread_mutex_lock(&q->lock);
    if (q->head - q->tail >= q->capacity) {
        pthread_mutex_unlock(&q->lock);
        return false;
    }
    memcpy(q->buf + (q->head & q->mask) * q->slot_size, data, q->slot_size);
    q->head++;
    pthread_mutex_unlock(&q->lock);
    return true;
}

static bool mq_pop(struct mq *q, void *data)
{
    pthread_mutex_lock(&q->lock);
    if (q->tail == q->head) {
        pthread_mutex_unlock(&q->lock);
        return false;
    }
    memcpy(data, q->buf + (q->tail & q->mask) * q->slot_size, q->slot_size);
    q->tail++;
    pthread_mutex_unlock(&q->lock);
    return true;
}

/* ---- Single-threaded cost: alternating push/pop, queue depth 1 ----
 *
 * No cross-core coherence traffic, so this isolates the instruction and
 * atomic cost of one push plus one pop. The slot index still advances each
 * iteration, so the cache behavior stays realistic. */

static double bench_single_ringbuf(size_t slot_size, size_t capacity,
                                   bool overwrite)
{
    double rate[REPS];
    char *msg = calloc(1, slot_size);
    char *out = calloc(1, slot_size);

    for (int r = 0; r < REPS; r++) {
        struct ringbuf *rb = ringbuf_create(slot_size, capacity, overwrite);
        /* Warm the slots so the timed loop is not measuring page faults. */
        for (unsigned i = 0; i < capacity; i++) {
            ringbuf_push(rb, msg);
            ringbuf_pop(rb, out);
        }

        uint64_t t0 = now_ns();
        for (unsigned i = 0; i < TP_MESSAGES; i++) {
            memcpy(msg, &i, sizeof(i));
            while (!ringbuf_push(rb, msg))
                cpu_relax();
            while (!ringbuf_pop(rb, out))
                cpu_relax();
            sink += (uint64_t)out[0];
        }
        uint64_t t1 = now_ns();

        rate[r] = (double)TP_MESSAGES * 1e9 / (double)(t1 - t0);
        ringbuf_destroy(rb);
    }

    free(msg);
    free(out);
    return median_dbl(rate, REPS);
}

static double bench_single_mutex(size_t slot_size, size_t capacity)
{
    double rate[REPS];
    char *msg = calloc(1, slot_size);
    char *out = calloc(1, slot_size);

    for (int r = 0; r < REPS; r++) {
        struct mq *q = mq_create(slot_size, capacity);
        for (unsigned i = 0; i < capacity; i++) {
            mq_push(q, msg);
            mq_pop(q, out);
        }

        uint64_t t0 = now_ns();
        for (unsigned i = 0; i < TP_MESSAGES; i++) {
            memcpy(msg, &i, sizeof(i));
            while (!mq_push(q, msg))
                cpu_relax();
            while (!mq_pop(q, out))
                cpu_relax();
            sink += (uint64_t)out[0];
        }
        uint64_t t1 = now_ns();

        rate[r] = (double)TP_MESSAGES * 1e9 / (double)(t1 - t0);
        mq_destroy(q);
    }

    free(msg);
    free(out);
    return median_dbl(rate, REPS);
}

/* ---- Two-thread throughput ---- */

struct tp_args {
    void *q;
    size_t slot_size;
    uint64_t count;
    atomic_bool *done;
    uint64_t received;
    uint64_t seq_errors;
};

static void *tp_ringbuf_producer(void *arg)
{
    struct tp_args *a = arg;
    pin_to(PRODUCER_CPU);
    char *msg = calloc(1, a->slot_size);

    for (uint64_t seq = 0; seq < a->count; seq++) {
        memcpy(msg, &seq, sizeof(seq));
        while (!ringbuf_push(a->q, msg))
            cpu_relax();
    }
    atomic_store_explicit(a->done, true, memory_order_release);
    free(msg);
    return NULL;
}

static void *tp_ringbuf_consumer(void *arg)
{
    struct tp_args *a = arg;
    pin_to(CONSUMER_CPU);
    char *out = calloc(1, a->slot_size);
    uint64_t expected = 0;

    for (;;) {
        if (ringbuf_pop(a->q, out)) {
            uint64_t seq;
            memcpy(&seq, out, sizeof(seq));
            /* Lossless mode must deliver every seq in order; a mismatch
             * means the benchmark is measuring a broken buffer. */
            if (seq != expected)
                a->seq_errors++;
            expected = seq + 1;
            a->received++;
            sink += seq;
            continue;
        }
        if (atomic_load_explicit(a->done, memory_order_acquire)) {
            if (!ringbuf_pop(a->q, out))
                break;
            uint64_t seq;
            memcpy(&seq, out, sizeof(seq));
            a->received++;
            sink += seq;
        } else {
            cpu_relax();
        }
    }
    free(out);
    return NULL;
}

static void *tp_mutex_producer(void *arg)
{
    struct tp_args *a = arg;
    pin_to(PRODUCER_CPU);
    char *msg = calloc(1, a->slot_size);

    for (uint64_t seq = 0; seq < a->count; seq++) {
        memcpy(msg, &seq, sizeof(seq));
        while (!mq_push(a->q, msg))
            cpu_relax();
    }
    atomic_store_explicit(a->done, true, memory_order_release);
    free(msg);
    return NULL;
}

static void *tp_mutex_consumer(void *arg)
{
    struct tp_args *a = arg;
    pin_to(CONSUMER_CPU);
    char *out = calloc(1, a->slot_size);

    for (;;) {
        if (mq_pop(a->q, out)) {
            a->received++;
            sink += (uint64_t)out[0];
            continue;
        }
        if (atomic_load_explicit(a->done, memory_order_acquire)) {
            if (!mq_pop(a->q, out))
                break;
            a->received++;
        } else {
            cpu_relax();
        }
    }
    free(out);
    return NULL;
}

/* Returns median messages/sec; reports drops via *dropped_out. */
static double bench_spsc(bool use_mutex, bool overwrite, size_t slot_size,
                         size_t capacity, uint64_t *dropped_out,
                         uint64_t *errors_out)
{
    double rate[REPS];
    uint64_t dropped = 0, errors = 0;

    for (int r = 0; r < REPS; r++) {
        void *q = use_mutex ?
                      (void *)mq_create(slot_size, capacity) :
                      (void *)ringbuf_create(slot_size, capacity, overwrite);
        atomic_bool done = false;
        struct tp_args pa = {
            .q = q, .slot_size = slot_size, .count = TP_MESSAGES, .done = &done
        };
        struct tp_args ca = {
            .q = q, .slot_size = slot_size, .count = TP_MESSAGES, .done = &done
        };

        pthread_t prod, cons;
        uint64_t t0 = now_ns();
        pthread_create(&prod, NULL,
                       use_mutex ? tp_mutex_producer : tp_ringbuf_producer,
                       &pa);
        pthread_create(&cons, NULL,
                       use_mutex ? tp_mutex_consumer : tp_ringbuf_consumer,
                       &ca);
        pthread_join(prod, NULL);
        pthread_join(cons, NULL);
        uint64_t t1 = now_ns();

        rate[r] = (double)TP_MESSAGES * 1e9 / (double)(t1 - t0);
        if (use_mutex) {
            mq_destroy(q);
        } else {
            dropped = ringbuf_dropped_count(q);
            errors = ca.seq_errors;
            ringbuf_destroy(q);
        }
    }

    if (dropped_out)
        *dropped_out = dropped;
    if (errors_out)
        *errors_out = errors;
    return median_dbl(rate, REPS);
}

/* ---- Latency: paced sends, timestamp in the payload ---- */

struct lat_args {
    void *q;
    size_t slot_size;
    bool use_mutex;
    uint64_t count;
    atomic_bool *done;
    uint64_t *samples;
    uint64_t received;
};

static void *lat_producer(void *arg)
{
    struct lat_args *a = arg;
    pin_to(PRODUCER_CPU);
    char *msg = calloc(1, a->slot_size);
    uint64_t next = now_ns();

    for (uint64_t seq = 0; seq < a->count; seq++) {
        next += LAT_GAP_NS;
        while (now_ns() < next)
            cpu_relax();

        uint64_t ts = now_ns();
        memcpy(msg, &seq, sizeof(seq));
        memcpy(msg + sizeof(seq), &ts, sizeof(ts));
        if (a->use_mutex) {
            while (!mq_push(a->q, msg))
                cpu_relax();
        } else {
            while (!ringbuf_push(a->q, msg))
                cpu_relax();
        }
    }
    atomic_store_explicit(a->done, true, memory_order_release);
    free(msg);
    return NULL;
}

static void *lat_consumer(void *arg)
{
    struct lat_args *a = arg;
    pin_to(CONSUMER_CPU);
    char *out = calloc(1, a->slot_size);

    for (;;) {
        bool got = a->use_mutex ? mq_pop(a->q, out) : ringbuf_pop(a->q, out);
        if (got) {
            uint64_t recv = now_ns();
            uint64_t ts;
            memcpy(&ts, out + sizeof(uint64_t), sizeof(ts));
            if (a->received < a->count)
                a->samples[a->received] = recv - ts;
            a->received++;
            continue;
        }
        if (atomic_load_explicit(a->done, memory_order_acquire)) {
            got = a->use_mutex ? mq_pop(a->q, out) : ringbuf_pop(a->q, out);
            if (!got)
                break;
            a->received++;
        } else {
            cpu_relax();
        }
    }
    free(out);
    return NULL;
}

static void bench_latency(const char *label, bool use_mutex, bool overwrite,
                          size_t slot_size, size_t capacity)
{
    void *q = use_mutex ?
                  (void *)mq_create(slot_size, capacity) :
                  (void *)ringbuf_create(slot_size, capacity, overwrite);
    uint64_t *samples = calloc(LAT_MESSAGES, sizeof(*samples));
    atomic_bool done = false;

    struct lat_args pa = { .q = q,
                           .slot_size = slot_size,
                           .use_mutex = use_mutex,
                           .count = LAT_MESSAGES,
                           .done = &done,
                           .samples = samples };
    struct lat_args ca = pa;

    pthread_t prod, cons;
    pthread_create(&prod, NULL, lat_producer, &pa);
    pthread_create(&cons, NULL, lat_consumer, &ca);
    pthread_join(prod, NULL);
    pthread_join(cons, NULL);

    uint64_t n = ca.received < LAT_MESSAGES ? ca.received : LAT_MESSAGES;
    if (n == 0) {
        printf("  %-26s no samples\n", label);
        goto out;
    }

    qsort(samples, n, sizeof(*samples), cmp_u64);
    double mean = 0;
    for (uint64_t i = 0; i < n; i++)
        mean += (double)samples[i];
    mean /= (double)n;

    printf("  %-26s %6" PRIu64 " %7" PRIu64 " %7" PRIu64 " %7" PRIu64
           " %8" PRIu64 " %8.0f\n",
           label, samples[0], samples[n / 2], samples[(uint64_t)(n * 0.99)],
           samples[(uint64_t)(n * 0.999)], samples[n - 1], mean);

out:
    free(samples);
    if (use_mutex)
        mq_destroy(q);
    else
        ringbuf_destroy(q);
}

static void print_row(const char *label, double rate, size_t slot_size)
{
    printf("  %-26s %9.2f %8.1f %9.1f\n", label, rate / 1e6, 1e9 / rate,
           rate * (double)slot_size / (1024 * 1024 * 1024));
}

int main(void)
{
    pin_to(PRODUCER_CPU);

    /* clock_gettime is on the measurement path for latency, so report its
     * cost -- it sets the floor on what the latency numbers can resolve. */
    uint64_t c0 = now_ns();
    for (int i = 0; i < 1000000; i++)
        sink += now_ns();
    uint64_t c1 = now_ns();
    printf("clock_gettime(CLOCK_MONOTONIC): %.1f ns/call\n\n",
           (double)(c1 - c0) / 1e6);

    printf("Single-threaded, alternating push/pop (depth 1), 64B slots,"
           " cap 1024\n");
    printf("  %-26s %9s %8s %9s\n", "", "Mmsg/s", "ns/op", "GiB/s");
    print_row("ringbuf lossless", bench_single_ringbuf(64, 1024, false), 64);
    print_row("ringbuf overwrite", bench_single_ringbuf(64, 1024, true), 64);
    print_row("mutex baseline", bench_single_mutex(64, 1024), 64);

    printf("\nSPSC two threads (cpu %d -> %d), 64B slots, by capacity\n",
           PRODUCER_CPU, CONSUMER_CPU);
    printf("  %-26s %9s %8s %9s\n", "", "Mmsg/s", "ns/msg", "GiB/s");
    size_t caps[] = { 16, 256, 4096, 65536 };
    for (unsigned i = 0; i < sizeof caps / sizeof *caps; i++) {
        char label[64];
        uint64_t errors = 0;
        snprintf(label, sizeof label, "lossless cap=%zu", caps[i]);
        double r = bench_spsc(false, false, 64, caps[i], NULL, &errors);
        print_row(label, r, 64);
        if (errors)
            printf("    !! %" PRIu64 " sequence errors\n", errors);
    }
    print_row("mutex cap=4096", bench_spsc(true, false, 64, 4096, NULL, NULL),
              64);

    printf("\nSPSC two threads, cap 4096, by slot size\n");
    printf("  %-26s %9s %8s %9s\n", "", "Mmsg/s", "ns/msg", "GiB/s");
    size_t sizes[] = { 16, 64, 256, 1024 };
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        char label[64];
        snprintf(label, sizeof label, "lossless slot=%zuB", sizes[i]);
        print_row(label, bench_spsc(false, false, sizes[i], 4096, NULL, NULL),
                  sizes[i]);
    }

    printf("\nSPSC two threads, overwrite mode (producer never blocks)\n");
    printf("  %-26s %9s %8s %9s\n", "", "Mmsg/s", "ns/msg", "GiB/s");
    for (unsigned i = 0; i < sizeof caps / sizeof *caps; i++) {
        char label[80];
        uint64_t dropped = 0;
        double r = bench_spsc(false, true, 64, caps[i], &dropped, NULL);
        snprintf(label, sizeof label, "overwrite cap=%zu", caps[i]);
        print_row(label, r, 64);
        printf("    dropped %.1f%% of %u\n",
               100.0 * (double)dropped / TP_MESSAGES, TP_MESSAGES);
    }

    printf("\nEnd-to-end handoff latency, 64B slots, cap 4096,"
           " one msg per %d ns (ns)\n",
           LAT_GAP_NS);
    printf("  %-26s %6s %7s %7s %7s %8s %8s\n", "", "min", "p50", "p99",
           "p99.9", "max", "mean");
    bench_latency("ringbuf lossless", false, false, 64, 4096);
    bench_latency("ringbuf overwrite", false, true, 64, 4096);
    bench_latency("mutex baseline", true, false, 64, 4096);

    return 0;
}
