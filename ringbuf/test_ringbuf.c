#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "ringbuf.h"

#define PAYLOAD_LEN 24

typedef struct {
    uint64_t seq;
    unsigned char payload[PAYLOAD_LEN];
} message_t;

static void fill_payload(unsigned char *payload, uint64_t seq) {
    for (int i = 0; i < PAYLOAD_LEN; i++)
        payload[i] = (unsigned char)((seq + (uint64_t)i) & 0xFF);
}

static bool payload_ok(const unsigned char *payload, uint64_t seq) {
    unsigned char expected[PAYLOAD_LEN];
    fill_payload(expected, seq);
    return memcmp(payload, expected, PAYLOAD_LEN) == 0;
}

/* Upper bound on consecutive failed push/pop attempts before a test declares
 * a stall. Legitimate contention resolves within a few thousand yields even
 * when the peer thread loses its timeslice, so this is a large safety margin
 * -- but it is bounded, so a regression that wedges the buffer (e.g. an empty
 * check that mis-reports forever) fails the suite instead of hanging it. */
#define MAX_SPIN 2000000u

/* ---- Lossless transfer (overwrite_on_full = false) ---- */

typedef struct {
    ringbuf_t *rb;
    uint64_t num_messages;
    bool failed;
    const char *label;
} lossless_producer_args_t;

typedef struct {
    ringbuf_t *rb;
    uint64_t num_messages;
    bool failed;
    const char *label;
} lossless_consumer_args_t;

static void *lossless_producer_main(void *arg) {
    lossless_producer_args_t *a = arg;
    for (uint64_t seq = 0; seq < a->num_messages; seq++) {
        message_t msg;
        msg.seq = seq;
        fill_payload(msg.payload, seq);

        uint64_t spins = 0;
        while (!ringbuf_push(a->rb, &msg)) {
            if (++spins > MAX_SPIN) {
                fprintf(stderr, "%s: producer stalled pushing seq %" PRIu64 "\n", a->label, seq);
                a->failed = true;
                return NULL;
            }
            sched_yield();
        }
    }
    return NULL;
}

static void *lossless_consumer_main(void *arg) {
    lossless_consumer_args_t *a = arg;
    for (uint64_t expected_seq = 0; expected_seq < a->num_messages; expected_seq++) {
        message_t msg;

        uint64_t spins = 0;
        while (!ringbuf_pop(a->rb, &msg)) {
            if (++spins > MAX_SPIN) {
                fprintf(stderr, "%s: consumer stalled waiting for seq %" PRIu64 "\n", a->label,
                        expected_seq);
                a->failed = true;
                return NULL;
            }
            sched_yield();
        }

        if (msg.seq != expected_seq || !payload_ok(msg.payload, expected_seq)) {
            fprintf(stderr, "%s: mismatch at expected seq %" PRIu64 ", got %" PRIu64 "\n",
                    a->label, expected_seq, msg.seq);
            a->failed = true;
            return NULL;
        }
    }
    return NULL;
}

/* Runs a two-thread lossless transfer over an already-created buffer. */
static bool run_lossless_transfer(ringbuf_t *rb, uint64_t n, const char *label) {
    lossless_producer_args_t pargs = {
        .rb = rb, .num_messages = n, .failed = false, .label = label};
    lossless_consumer_args_t cargs = {
        .rb = rb, .num_messages = n, .failed = false, .label = label};

    pthread_t producer, consumer;
    pthread_create(&producer, NULL, lossless_producer_main, &pargs);
    pthread_create(&consumer, NULL, lossless_consumer_main, &cargs);
    pthread_join(producer, NULL);
    pthread_join(consumer, NULL);

    if (pargs.failed || cargs.failed)
        return false;

    printf("%s: PASS (%" PRIu64 " messages)\n", label, n);
    return true;
}

static bool run_lossless_test(void) {
    /* Small, non-trivial capacity forces heavy wraparound during the run. */
    ringbuf_t *rb = ringbuf_create(sizeof(message_t), 16, false);
    if (!rb) {
        fprintf(stderr, "lossless: ringbuf_create failed\n");
        return false;
    }
    bool ok = run_lossless_transfer(rb, 2000000u, "lossless");
    ringbuf_destroy(rb);
    return ok;
}

/* ---- Overwrite mode, single-threaded eviction check ---- */

/* Expects a capacity-4 buffer created with overwrite_on_full = true. Pushes
 * 10 messages with no pops, so the oldest 6 must be evicted and only seq
 * 6..9 remain. */
static bool run_overwrite_basic_on(ringbuf_t *rb, const char *label) {
    for (uint64_t seq = 0; seq < 10; seq++) {
        message_t msg;
        msg.seq = seq;
        fill_payload(msg.payload, seq);
        if (!ringbuf_push(rb, &msg)) {
            fprintf(stderr, "%s: push unexpectedly failed at seq %" PRIu64 "\n", label, seq);
            return false;
        }
    }

    size_t dropped = ringbuf_dropped_count(rb);
    if (dropped != 6) {
        fprintf(stderr, "%s: expected 6 dropped, got %zu\n", label, dropped);
        return false;
    }

    for (uint64_t expected_seq = 6; expected_seq < 10; expected_seq++) {
        message_t msg;
        if (!ringbuf_pop(rb, &msg)) {
            fprintf(stderr, "%s: pop unexpectedly failed at seq %" PRIu64 "\n", label,
                    expected_seq);
            return false;
        }
        if (msg.seq != expected_seq || !payload_ok(msg.payload, expected_seq)) {
            fprintf(stderr, "%s: mismatch, expected seq %" PRIu64 ", got %" PRIu64 "\n", label,
                    expected_seq, msg.seq);
            return false;
        }
    }

    message_t msg;
    if (ringbuf_pop(rb, &msg)) {
        fprintf(stderr, "%s: expected empty buffer after draining\n", label);
        return false;
    }

    printf("%s: PASS (dropped=%zu)\n", label, dropped);
    return true;
}

static bool run_overwrite_basic_test(void) {
    ringbuf_t *rb = ringbuf_create(sizeof(message_t), 4, true);
    if (!rb) {
        fprintf(stderr, "overwrite_basic: ringbuf_create failed\n");
        return false;
    }
    bool ok = run_overwrite_basic_on(rb, "overwrite_basic");
    ringbuf_destroy(rb);
    return ok;
}

/* ---- Overwrite mode, concurrent stress. A tiny capacity maximizes
 * eviction-vs-read contention on the shared boundary slot. ---- */

typedef struct {
    ringbuf_t *rb;
    uint64_t num_messages;
    atomic_bool *done;
    bool failed;
    const char *label;
} stress_producer_args_t;

typedef struct {
    ringbuf_t *rb;
    atomic_bool *done;
    uint64_t received;
    uint64_t last_seq;
    bool corrupted;
    bool out_of_order;
    bool stalled;
    const char *label;
} stress_consumer_args_t;

static void *stress_producer_main(void *arg) {
    stress_producer_args_t *a = arg;
    for (uint64_t seq = 0; seq < a->num_messages; seq++) {
        message_t msg;
        msg.seq = seq;
        fill_payload(msg.payload, seq);

        /* In overwrite mode, push only fails on the rare transient lock
         * contention with an in-flight pop on the exact oldest slot. */
        uint64_t spins = 0;
        while (!ringbuf_push(a->rb, &msg)) {
            if (++spins > MAX_SPIN) {
                fprintf(stderr, "%s: producer stalled pushing seq %" PRIu64 "\n", a->label, seq);
                a->failed = true;
                atomic_store_explicit(a->done, true, memory_order_release);
                return NULL;
            }
            sched_yield();
        }
    }
    atomic_store_explicit(a->done, true, memory_order_release);
    return NULL;
}

static void *stress_consumer_main(void *arg) {
    stress_consumer_args_t *a = arg;
    bool have_prev = false;
    uint64_t prev_seq = 0;
    uint64_t spins = 0;

    for (;;) {
        message_t msg;
        if (!ringbuf_pop(a->rb, &msg)) {
            if (atomic_load_explicit(a->done, memory_order_acquire)) {
                if (!ringbuf_pop(a->rb, &msg))
                    break; /* producer finished and buffer is fully drained */
            } else {
                if (++spins > MAX_SPIN) {
                    fprintf(stderr, "%s: consumer stalled after %" PRIu64 " messages\n", a->label,
                            a->received);
                    a->stalled = true;
                    return NULL;
                }
                sched_yield();
                continue;
            }
        }
        spins = 0;

        if (!payload_ok(msg.payload, msg.seq)) {
            a->corrupted = true;
            break;
        }
        if (have_prev && msg.seq <= prev_seq) {
            a->out_of_order = true;
            break;
        }

        have_prev = true;
        prev_seq = msg.seq;
        a->received++;
        a->last_seq = msg.seq;
    }
    return NULL;
}

static bool run_overwrite_stress_on(ringbuf_t *rb, uint64_t n, const char *label) {
    atomic_bool done = false;
    stress_producer_args_t pargs = {
        .rb = rb, .num_messages = n, .done = &done, .failed = false, .label = label};
    stress_consumer_args_t cargs = {.rb = rb,
                                    .done = &done,
                                    .received = 0,
                                    .last_seq = 0,
                                    .corrupted = false,
                                    .out_of_order = false,
                                    .stalled = false,
                                    .label = label};

    pthread_t producer, consumer;
    pthread_create(&producer, NULL, stress_producer_main, &pargs);
    pthread_create(&consumer, NULL, stress_consumer_main, &cargs);
    pthread_join(producer, NULL);
    pthread_join(consumer, NULL);

    size_t dropped = ringbuf_dropped_count(rb);

    if (pargs.failed || cargs.stalled)
        return false;
    if (cargs.corrupted) {
        fprintf(stderr, "%s: payload corruption detected\n", label);
        return false;
    }
    if (cargs.out_of_order) {
        fprintf(stderr, "%s: out-of-order/duplicate seq detected\n", label);
        return false;
    }
    if (cargs.last_seq != n - 1) {
        fprintf(stderr, "%s: did not observe final message (last_seq=%" PRIu64 ")\n", label,
                cargs.last_seq);
        return false;
    }
    if (cargs.received + dropped != n) {
        fprintf(stderr, "%s: received(%" PRIu64 ") + dropped(%zu) != pushed(%" PRIu64 ")\n",
                label, cargs.received, dropped, n);
        return false;
    }

    printf("%s: PASS (received=%" PRIu64 ", dropped=%zu)\n", label, cargs.received, dropped);
    return true;
}

static bool run_overwrite_stress_test(void) {
    ringbuf_t *rb = ringbuf_create(sizeof(message_t), 4, true);
    if (!rb) {
        fprintf(stderr, "overwrite_stress: ringbuf_create failed\n");
        return false;
    }
    bool ok = run_overwrite_stress_on(rb, 2000000u, "overwrite_stress");
    ringbuf_destroy(rb);
    return ok;
}

#ifdef RINGBUF_TEST_HOOKS

/* ---- Counter-overflow tests.
 *
 * The internal counters are monotonic and only mapped onto slots via a mask,
 * so they eventually overflow SIZE_MAX. Reaching that organically would take
 * 2^64 pushes, so these tests park the counters just below the boundary with
 * ringbuf_test_seed_counters() and then push across it.
 *
 * SEED_BASE leaves room for only 2 increments before write_idx wraps, so
 * every test below ends up with write_idx wrapped to a small value while
 * read_idx is still near SIZE_MAX. That is precisely the state an absolute
 * `read_idx >= cached_write` empty check mis-reports as permanently empty.
 * ---- */

#define SEED_BASE (SIZE_MAX - 2)

/* Sharp regression test: single-threaded, so a failure can only come from
 * the full/empty arithmetic rather than from a race. */
static bool run_wrap_boundary_test(void) {
    const uint64_t capacity = 16;
    ringbuf_t *rb = ringbuf_create(sizeof(message_t), (size_t)capacity, false);
    if (!rb) {
        fprintf(stderr, "wrap_boundary: ringbuf_create failed\n");
        return false;
    }
    ringbuf_test_seed_counters(rb, SEED_BASE);

    if (!ringbuf_is_empty_approx(rb)) {
        fprintf(stderr, "wrap_boundary: should read empty right after seeding\n");
        goto fail;
    }

    /* Fill to exactly capacity; write_idx crosses SIZE_MAX partway through. */
    for (uint64_t seq = 0; seq < capacity; seq++) {
        message_t msg;
        msg.seq = seq;
        fill_payload(msg.payload, seq);
        if (!ringbuf_push(rb, &msg)) {
            fprintf(stderr, "wrap_boundary: push failed at seq %" PRIu64 " (expected room)\n",
                    seq);
            goto fail;
        }
    }

    if (!ringbuf_is_full_approx(rb)) {
        fprintf(stderr, "wrap_boundary: should read full after %" PRIu64 " pushes\n", capacity);
        goto fail;
    }

    /* Non-overwrite buffer: the next push must be refused, not wrap around
     * and clobber unread data. */
    {
        message_t msg;
        msg.seq = capacity;
        fill_payload(msg.payload, capacity);
        if (ringbuf_push(rb, &msg)) {
            fprintf(stderr, "wrap_boundary: push succeeded on a full buffer\n");
            goto fail;
        }
    }

    /* write_idx has now wrapped past SIZE_MAX while read_idx has not. */
    for (uint64_t expected_seq = 0; expected_seq < capacity; expected_seq++) {
        message_t msg;
        if (!ringbuf_pop(rb, &msg)) {
            fprintf(stderr,
                    "wrap_boundary: pop reported empty across the SIZE_MAX boundary "
                    "at seq %" PRIu64 "\n",
                    expected_seq);
            goto fail;
        }
        if (msg.seq != expected_seq || !payload_ok(msg.payload, expected_seq)) {
            fprintf(stderr, "wrap_boundary: mismatch, expected seq %" PRIu64 ", got %" PRIu64 "\n",
                    expected_seq, msg.seq);
            goto fail;
        }
    }

    {
        message_t msg;
        if (ringbuf_pop(rb, &msg)) {
            fprintf(stderr, "wrap_boundary: expected empty buffer after draining\n");
            goto fail;
        }
    }
    if (!ringbuf_is_empty_approx(rb)) {
        fprintf(stderr, "wrap_boundary: should read empty after draining\n");
        goto fail;
    }

    ringbuf_destroy(rb);
    printf("wrap_boundary: PASS (crossed SIZE_MAX, %" PRIu64 " messages)\n", capacity);
    return true;

fail:
    ringbuf_destroy(rb);
    return false;
}

/* Overwrite-mode eviction accounting across the same boundary. */
static bool run_wrap_overwrite_basic_test(void) {
    ringbuf_t *rb = ringbuf_create(sizeof(message_t), 4, true);
    if (!rb) {
        fprintf(stderr, "wrap_overwrite_basic: ringbuf_create failed\n");
        return false;
    }
    ringbuf_test_seed_counters(rb, SEED_BASE);
    bool ok = run_overwrite_basic_on(rb, "wrap_overwrite_basic");
    ringbuf_destroy(rb);
    return ok;
}

/* Concurrent coverage of the boundary, for ThreadSanitizer. Smaller counts
 * than the main stress tests: the boundary is crossed in the first handful
 * of pushes, so the rest is just ordinary running. */
static bool run_wrap_lossless_concurrent_test(void) {
    ringbuf_t *rb = ringbuf_create(sizeof(message_t), 16, false);
    if (!rb) {
        fprintf(stderr, "wrap_lossless_concurrent: ringbuf_create failed\n");
        return false;
    }
    ringbuf_test_seed_counters(rb, SEED_BASE);
    bool ok = run_lossless_transfer(rb, 200000u, "wrap_lossless_concurrent");
    ringbuf_destroy(rb);
    return ok;
}

static bool run_wrap_overwrite_stress_test(void) {
    ringbuf_t *rb = ringbuf_create(sizeof(message_t), 4, true);
    if (!rb) {
        fprintf(stderr, "wrap_overwrite_stress: ringbuf_create failed\n");
        return false;
    }
    ringbuf_test_seed_counters(rb, SEED_BASE);
    bool ok = run_overwrite_stress_on(rb, 200000u, "wrap_overwrite_stress");
    ringbuf_destroy(rb);
    return ok;
}

#endif /* RINGBUF_TEST_HOOKS */

int main(void) {
    bool ok = true;
    ok &= run_lossless_test();
    ok &= run_overwrite_basic_test();
    ok &= run_overwrite_stress_test();
#ifdef RINGBUF_TEST_HOOKS
    ok &= run_wrap_boundary_test();
    ok &= run_wrap_overwrite_basic_test();
    ok &= run_wrap_lossless_concurrent_test();
    ok &= run_wrap_overwrite_stress_test();
#else
    printf("counter-overflow tests SKIPPED (build with -DRINGBUF_TEST_HOOKS)\n");
#endif

    if (!ok) {
        printf("FAIL\n");
        return 1;
    }

    printf("PASS\n");
    return 0;
}
