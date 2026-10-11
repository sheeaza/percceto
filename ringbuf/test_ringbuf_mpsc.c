#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ringbuf_mpsc.h"

#define PAYLOAD_LEN 24

/* Multi-producer messages carry their origin, because a single global
 * sequence order no longer exists: the interleaving of two producers'
 * tickets is genuinely nondeterministic. Each producer numbers its own
 * stream, and the consumer checks the streams independently. */
struct message {
    uint64_t seq;
    uint32_t producer_id;
    unsigned char payload[PAYLOAD_LEN];
};

/**
 * fill_payload - write the seq-derived payload pattern
 * @payload: PAYLOAD_LEN bytes to fill
 * @seq: sequence number the pattern is derived from
 */
static void fill_payload(unsigned char *payload, uint64_t seq)
{
    for (int i = 0; i < PAYLOAD_LEN; i++)
        payload[i] = (unsigned char)((seq + (uint64_t)i) & 0xFF);
}

/**
 * payload_ok - check a payload against the pattern for a sequence number
 * @payload: PAYLOAD_LEN bytes as received
 * @seq: sequence number the payload should match
 *
 * Return: true if @payload is exactly what fill_payload() writes for @seq.
 */
static bool payload_ok(const unsigned char *payload, uint64_t seq)
{
    unsigned char expected[PAYLOAD_LEN];
    fill_payload(expected, seq);
    return memcmp(payload, expected, PAYLOAD_LEN) == 0;
}

/* Upper bound on consecutive failed push/pop attempts before a test declares
 * a stall. Legitimate contention resolves within a few thousand yields even
 * when a peer thread loses its timeslice, so this is a large safety margin
 * -- but it is bounded, so a regression that wedges the buffer (e.g. a
 * head-of-line message that is never published) fails the suite instead of
 * hanging it. */
#define MAX_SPIN 2000000u

/* Producer count for the three multi-producer cases. Four threads on the
 * same ring is enough to keep the ticket CAS genuinely contended. */
#define NUM_PRODUCERS 4

/* ---- Lossless transfer (overwrite_on_full = false) ----
 *
 * Single-producer, so these mirror test_ringbuf.c case for case and stay
 * directly comparable with it. The multi-producer cases are further down. */

struct lossless_producer_args {
    struct ringbuf_mpsc *rb;
    uint64_t num_messages;
    bool failed;
    const char *label;
};

struct lossless_consumer_args {
    struct ringbuf_mpsc *rb;
    uint64_t num_messages;
    bool failed;
    const char *label;
};

/**
 * lossless_producer_main - push num_messages sequenced messages
 * @arg: struct lossless_producer_args
 *
 * Return: NULL always; failure is reported via the args' @failed field.
 */
static void *lossless_producer_main(void *arg)
{
    struct lossless_producer_args *a = arg;
    for (uint64_t seq = 0; seq < a->num_messages; seq++) {
        struct message msg;
        msg.seq = seq;
        msg.producer_id = 0;
        fill_payload(msg.payload, seq);

        uint64_t spins = 0;
        while (!ringbuf_mpsc_push(a->rb, &msg)) {
            if (++spins > MAX_SPIN) {
                fprintf(stderr,
                        "%s: producer stalled pushing seq %" PRIu64 "\n",
                        a->label, seq);
                a->failed = true;
                return NULL;
            }
            sched_yield();
        }
    }
    return NULL;
}

/**
 * lossless_consumer_main - pop num_messages and check seq order and payload
 * @arg: struct lossless_consumer_args
 *
 * Return: NULL always; failure is reported via the args' @failed field.
 */
static void *lossless_consumer_main(void *arg)
{
    struct lossless_consumer_args *a = arg;
    for (uint64_t expected_seq = 0; expected_seq < a->num_messages;
         expected_seq++) {
        struct message msg;

        uint64_t spins = 0;
        while (!ringbuf_mpsc_pop(a->rb, &msg)) {
            if (++spins > MAX_SPIN) {
                fprintf(stderr,
                        "%s: consumer stalled waiting for seq %" PRIu64 "\n",
                        a->label, expected_seq);
                a->failed = true;
                return NULL;
            }
            sched_yield();
        }

        if (msg.seq != expected_seq || !payload_ok(msg.payload, expected_seq)) {
            fprintf(stderr,
                    "%s: mismatch at expected seq %" PRIu64 ", got %" PRIu64
                    "\n",
                    a->label, expected_seq, msg.seq);
            a->failed = true;
            return NULL;
        }
    }
    return NULL;
}

/**
 * run_lossless_transfer - two-thread lossless transfer over an existing buffer
 * @rb: buffer created with overwrite_on_full = false
 * @n: number of messages to send
 * @label: name used in progress and failure messages
 *
 * Return: true if every message arrived in order with an intact payload.
 */
static bool run_lossless_transfer(struct ringbuf_mpsc *rb, uint64_t n,
                                  const char *label)
{
    struct lossless_producer_args pargs = {
        .rb = rb, .num_messages = n, .failed = false, .label = label
    };
    struct lossless_consumer_args cargs = {
        .rb = rb, .num_messages = n, .failed = false, .label = label
    };

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

/**
 * run_lossless_test - lossless mode, two threads, heavy wraparound
 *
 * Return: true on pass.
 */
static bool run_lossless_test(void)
{
    /* Small, non-trivial capacity forces heavy wraparound during the run. */
    struct ringbuf_mpsc *rb =
        ringbuf_mpsc_create(sizeof(struct message), 16, false);
    if (!rb) {
        fprintf(stderr, "lossless: ringbuf_mpsc_create failed\n");
        return false;
    }
    bool ok = run_lossless_transfer(rb, 2000000u, "lossless");
    ringbuf_mpsc_destroy(rb);
    return ok;
}

/* ---- Overwrite mode, single-threaded eviction check ---- */

/**
 * run_overwrite_basic_on - single-threaded eviction check
 * @rb: capacity-4 buffer created with overwrite_on_full = true
 * @label: name used in progress and failure messages
 *
 * Pushes 10 messages with no pops, so the oldest 6 must be evicted and
 * only seq 6..9 remain.
 *
 * Return: true if the drop count and the four surviving messages match.
 */
static bool run_overwrite_basic_on(struct ringbuf_mpsc *rb, const char *label)
{
    for (uint64_t seq = 0; seq < 10; seq++) {
        struct message msg;
        msg.seq = seq;
        msg.producer_id = 0;
        fill_payload(msg.payload, seq);
        if (!ringbuf_mpsc_push(rb, &msg)) {
            fprintf(stderr, "%s: push unexpectedly failed at seq %" PRIu64 "\n",
                    label, seq);
            return false;
        }
    }

    size_t dropped = ringbuf_mpsc_dropped_count(rb);
    if (dropped != 6) {
        fprintf(stderr, "%s: expected 6 dropped, got %zu\n", label, dropped);
        return false;
    }

    for (uint64_t expected_seq = 6; expected_seq < 10; expected_seq++) {
        struct message msg;
        if (!ringbuf_mpsc_pop(rb, &msg)) {
            fprintf(stderr, "%s: pop unexpectedly failed at seq %" PRIu64 "\n",
                    label, expected_seq);
            return false;
        }
        if (msg.seq != expected_seq || !payload_ok(msg.payload, expected_seq)) {
            fprintf(stderr,
                    "%s: mismatch, expected seq %" PRIu64 ", got %" PRIu64 "\n",
                    label, expected_seq, msg.seq);
            return false;
        }
    }

    struct message msg;
    if (ringbuf_mpsc_pop(rb, &msg)) {
        fprintf(stderr, "%s: expected empty buffer after draining\n", label);
        return false;
    }

    printf("%s: PASS (dropped=%zu)\n", label, dropped);
    return true;
}

/**
 * run_overwrite_basic_test - run the eviction check on a fresh buffer
 *
 * Return: true on pass.
 */
static bool run_overwrite_basic_test(void)
{
    struct ringbuf_mpsc *rb =
        ringbuf_mpsc_create(sizeof(struct message), 4, true);
    if (!rb) {
        fprintf(stderr, "overwrite_basic: ringbuf_mpsc_create failed\n");
        return false;
    }
    bool ok = run_overwrite_basic_on(rb, "overwrite_basic");
    ringbuf_mpsc_destroy(rb);
    return ok;
}

/* ---- Overwrite mode, concurrent stress. A tiny capacity maximizes
 * eviction-vs-read contention on the shared boundary slot. ---- */

struct stress_producer_args {
    struct ringbuf_mpsc *rb;
    uint64_t num_messages;
    atomic_bool *done;
    bool failed;
    const char *label;
};

struct stress_consumer_args {
    struct ringbuf_mpsc *rb;
    atomic_bool *done;
    uint64_t received;
    uint64_t last_seq;
    bool corrupted;
    bool out_of_order;
    bool stalled;
    const char *label;
};

/**
 * stress_producer_main - push num_messages into an overwrite-mode buffer
 * @arg: struct stress_producer_args
 *
 * Sets *done once finished, so the consumer knows when to drain and stop.
 *
 * Return: NULL always; failure is reported via the args' @failed field.
 */
static void *stress_producer_main(void *arg)
{
    struct stress_producer_args *a = arg;
    for (uint64_t seq = 0; seq < a->num_messages; seq++) {
        struct message msg;
        msg.seq = seq;
        msg.producer_id = 0;
        fill_payload(msg.payload, seq);

        /* In overwrite mode, push only fails on the rare transient lock
         * contention with an in-flight pop on the exact oldest slot. */
        uint64_t spins = 0;
        while (!ringbuf_mpsc_push(a->rb, &msg)) {
            if (++spins > MAX_SPIN) {
                fprintf(stderr,
                        "%s: producer stalled pushing seq %" PRIu64 "\n",
                        a->label, seq);
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

/**
 * stress_consumer_main - drain an overwrite-mode buffer until the producer
 *	is done, checking payloads and monotonic seq
 * @arg: struct stress_consumer_args
 *
 * Gaps in seq are expected -- those are evictions -- but a repeat or a
 * step backwards means a message was handed out twice.
 *
 * Return: NULL always; results and failures land in the args.
 */
static void *stress_consumer_main(void *arg)
{
    struct stress_consumer_args *a = arg;
    bool have_prev = false;
    uint64_t prev_seq = 0;
    uint64_t spins = 0;

    for (;;) {
        struct message msg;
        if (!ringbuf_mpsc_pop(a->rb, &msg)) {
            if (atomic_load_explicit(a->done, memory_order_acquire)) {
                if (!ringbuf_mpsc_pop(a->rb, &msg))
                    break; /* producer finished and buffer is fully drained */
            } else {
                if (++spins > MAX_SPIN) {
                    fprintf(stderr,
                            "%s: consumer stalled after %" PRIu64 " messages\n",
                            a->label, a->received);
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

/**
 * run_overwrite_stress_on - concurrent eviction stress over an existing buffer
 * @rb: buffer created with overwrite_on_full = true
 * @n: number of messages to push
 * @label: name used in progress and failure messages
 *
 * Return: true if payloads were intact, seqs strictly increasing, the final
 * message observed, and received + dropped == @n.
 */
static bool run_overwrite_stress_on(struct ringbuf_mpsc *rb, uint64_t n,
                                    const char *label)
{
    atomic_bool done = false;
    struct stress_producer_args pargs = { .rb = rb,
                                          .num_messages = n,
                                          .done = &done,
                                          .failed = false,
                                          .label = label };
    struct stress_consumer_args cargs = { .rb = rb,
                                          .done = &done,
                                          .received = 0,
                                          .last_seq = 0,
                                          .corrupted = false,
                                          .out_of_order = false,
                                          .stalled = false,
                                          .label = label };

    pthread_t producer, consumer;
    pthread_create(&producer, NULL, stress_producer_main, &pargs);
    pthread_create(&consumer, NULL, stress_consumer_main, &cargs);
    pthread_join(producer, NULL);
    pthread_join(consumer, NULL);

    size_t dropped = ringbuf_mpsc_dropped_count(rb);

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
        fprintf(stderr,
                "%s: did not observe final message (last_seq=%" PRIu64 ")\n",
                label, cargs.last_seq);
        return false;
    }
    if (cargs.received + dropped != n) {
        fprintf(stderr,
                "%s: received(%" PRIu64 ") + dropped(%zu) != pushed(%" PRIu64
                ")\n",
                label, cargs.received, dropped, n);
        return false;
    }

    printf("%s: PASS (received=%" PRIu64 ", dropped=%zu)\n", label,
           cargs.received, dropped);
    return true;
}

/**
 * run_overwrite_stress_test - run the eviction stress on a fresh buffer
 *
 * Return: true on pass.
 */
static bool run_overwrite_stress_test(void)
{
    struct ringbuf_mpsc *rb =
        ringbuf_mpsc_create(sizeof(struct message), 4, true);
    if (!rb) {
        fprintf(stderr, "overwrite_stress: ringbuf_mpsc_create failed\n");
        return false;
    }
    bool ok = run_overwrite_stress_on(rb, 2000000u, "overwrite_stress");
    ringbuf_mpsc_destroy(rb);
    return ok;
}

/* ---- Multi-producer cases, the ones specific to this variant ----
 *
 * NUM_PRODUCERS threads push concurrently, each numbering its own stream.
 * The consumer cannot check a single global order -- which producer wins a
 * given ticket is nondeterministic -- so it checks, per producer, that seqs
 * arrive strictly increasing. That is the property the ticket scheme must
 * provide and the one a broken CAS destroys: a duplicated or reordered
 * delivery shows up as a seq that is not greater than the last one seen
 * from that same producer. */

struct mp_producer_args {
    struct ringbuf_mpsc *rb;
    uint32_t producer_id;
    uint64_t num_messages;
    atomic_int *active; /* producers still running; consumer watches it */
    bool failed;
    const char *label;
};

struct mp_consumer_args {
    struct ringbuf_mpsc *rb;
    atomic_int *active;
    uint64_t num_messages; /* per producer */
    uint64_t received;
    /* Per-producer bookkeeping, indexed by producer_id. */
    uint64_t last_seq[NUM_PRODUCERS];
    uint64_t count[NUM_PRODUCERS];
    bool have_prev[NUM_PRODUCERS];
    bool corrupted;
    bool out_of_order;
    bool bad_producer_id;
    bool stalled;
    const char *label;
};

/**
 * mp_producer_main - push one producer's numbered stream
 * @arg: struct mp_producer_args
 *
 * Decrements *active on the way out, so the consumer can tell when every
 * producer has finished rather than just one.
 *
 * Return: NULL always; failure is reported via the args' @failed field.
 */
static void *mp_producer_main(void *arg)
{
    struct mp_producer_args *a = arg;
    for (uint64_t seq = 0; seq < a->num_messages; seq++) {
        struct message msg;
        msg.seq = seq;
        msg.producer_id = a->producer_id;
        fill_payload(msg.payload, seq);

        uint64_t spins = 0;
        while (!ringbuf_mpsc_push(a->rb, &msg)) {
            if (++spins > MAX_SPIN) {
                fprintf(stderr,
                        "%s: producer %" PRIu32 " stalled pushing seq %" PRIu64
                        "\n",
                        a->label, a->producer_id, seq);
                a->failed = true;
                atomic_fetch_sub_explicit(a->active, 1, memory_order_release);
                return NULL;
            }
            sched_yield();
        }
    }
    atomic_fetch_sub_explicit(a->active, 1, memory_order_release);
    return NULL;
}

/**
 * mp_consumer_main - drain a multi-producer buffer, checking each stream
 * @arg: struct mp_consumer_args
 *
 * Runs until every producer has finished and the ring is drained. Keeps a
 * per-producer last_seq, so interleaving between producers is allowed but a
 * repeat or step backwards within one producer's stream is not.
 *
 * Return: NULL always; results and failures land in the args.
 */
static void *mp_consumer_main(void *arg)
{
    struct mp_consumer_args *a = arg;
    uint64_t spins = 0;

    for (;;) {
        struct message msg;
        if (!ringbuf_mpsc_pop(a->rb, &msg)) {
            /* A false return does not mean empty here -- a producer may
             * hold the oldest ticket mid-publish. Only stop once every
             * producer is gone AND a final attempt still yields nothing. */
            if (atomic_load_explicit(a->active, memory_order_acquire) == 0) {
                if (!ringbuf_mpsc_pop(a->rb, &msg))
                    break;
            } else {
                if (++spins > MAX_SPIN) {
                    fprintf(stderr,
                            "%s: consumer stalled after %" PRIu64 " messages\n",
                            a->label, a->received);
                    a->stalled = true;
                    return NULL;
                }
                sched_yield();
                continue;
            }
        }
        spins = 0;

        if (msg.producer_id >= NUM_PRODUCERS) {
            a->bad_producer_id = true;
            break;
        }
        if (!payload_ok(msg.payload, msg.seq)) {
            a->corrupted = true;
            break;
        }

        uint32_t p = msg.producer_id;
        if (a->have_prev[p] && msg.seq <= a->last_seq[p]) {
            fprintf(stderr,
                    "%s: producer %" PRIu32 " seq %" PRIu64
                    " not after %" PRIu64 "\n",
                    a->label, p, msg.seq, a->last_seq[p]);
            a->out_of_order = true;
            break;
        }

        a->have_prev[p] = true;
        a->last_seq[p] = msg.seq;
        a->count[p]++;
        a->received++;
    }
    return NULL;
}

/**
 * run_mp_stress_on - concurrent multi-producer run over an existing buffer
 * @rb: the buffer, in either mode
 * @n: messages per producer
 * @lossless: true if the buffer was created with overwrite_on_full = false,
 *	in which case nothing may be dropped and every stream must arrive
 *	complete
 * @label: name used in progress and failure messages
 *
 * Return: true if every stream was strictly increasing with intact
 * payloads, and the totals reconcile.
 */
static bool run_mp_stress_on(struct ringbuf_mpsc *rb, uint64_t n, bool lossless,
                             const char *label)
{
    atomic_int active = NUM_PRODUCERS;
    struct mp_producer_args pargs[NUM_PRODUCERS];
    struct mp_consumer_args cargs = { .rb = rb,
                                      .active = &active,
                                      .num_messages = n,
                                      .received = 0,
                                      .last_seq = { 0 },
                                      .count = { 0 },
                                      .have_prev = { false },
                                      .corrupted = false,
                                      .out_of_order = false,
                                      .bad_producer_id = false,
                                      .stalled = false,
                                      .label = label };

    pthread_t producers[NUM_PRODUCERS], consumer;
    pthread_create(&consumer, NULL, mp_consumer_main, &cargs);
    for (uint32_t i = 0; i < NUM_PRODUCERS; i++) {
        pargs[i] = (struct mp_producer_args){ .rb = rb,
                                              .producer_id = i,
                                              .num_messages = n,
                                              .active = &active,
                                              .failed = false,
                                              .label = label };
        pthread_create(&producers[i], NULL, mp_producer_main, &pargs[i]);
    }
    for (uint32_t i = 0; i < NUM_PRODUCERS; i++)
        pthread_join(producers[i], NULL);
    pthread_join(consumer, NULL);

    size_t dropped = ringbuf_mpsc_dropped_count(rb);
    uint64_t pushed = n * NUM_PRODUCERS;

    for (uint32_t i = 0; i < NUM_PRODUCERS; i++)
        if (pargs[i].failed)
            return false;
    if (cargs.stalled)
        return false;
    if (cargs.bad_producer_id) {
        fprintf(stderr, "%s: message with out-of-range producer_id\n", label);
        return false;
    }
    if (cargs.corrupted) {
        fprintf(stderr, "%s: payload corruption detected\n", label);
        return false;
    }
    if (cargs.out_of_order) {
        fprintf(stderr, "%s: out-of-order/duplicate seq within a stream\n",
                label);
        return false;
    }
    if (cargs.received + dropped != pushed) {
        fprintf(stderr,
                "%s: received(%" PRIu64 ") + dropped(%zu) != pushed(%" PRIu64
                ")\n",
                label, cargs.received, dropped, pushed);
        return false;
    }

    if (lossless) {
        if (dropped != 0) {
            fprintf(stderr, "%s: lossless buffer dropped %zu\n", label,
                    dropped);
            return false;
        }
        /* Nothing may be lost, so each stream must have arrived whole --
         * and since seqs are strictly increasing per producer and start at
         * 0, a full count means exactly 0..n-1 with no gaps. */
        for (uint32_t i = 0; i < NUM_PRODUCERS; i++) {
            if (cargs.count[i] != n) {
                fprintf(stderr,
                        "%s: producer %" PRIu32 " delivered %" PRIu64
                        "/%" PRIu64 "\n",
                        label, i, cargs.count[i], n);
                return false;
            }
        }
    }

    printf("%s: PASS (%d producers, received=%" PRIu64 ", dropped=%zu)\n",
           label, NUM_PRODUCERS, cargs.received, dropped);
    return true;
}

/**
 * run_mp_lossless_test - NUM_PRODUCERS concurrent producers, nothing dropped
 *
 * The headline case for this variant: the one the single-writer write_idx of
 * ringbuf.c could not support at all.
 *
 * Return: true on pass.
 */
static bool run_mp_lossless_test(void)
{
    struct ringbuf_mpsc *rb =
        ringbuf_mpsc_create(sizeof(struct message), 16, false);
    if (!rb) {
        fprintf(stderr, "mpsc_lossless: ringbuf_mpsc_create failed\n");
        return false;
    }
    bool ok = run_mp_stress_on(rb, 500000u, true, "mpsc_lossless");
    ringbuf_mpsc_destroy(rb);
    return ok;
}

/**
 * run_mp_overwrite_stress_test - NUM_PRODUCERS producers against a tiny
 *	overwrite-mode ring
 *
 * Capacity 4 with four producers means almost every push evicts, which is
 * the maximum-contention case for the shared stamp CAS.
 *
 * Return: true on pass.
 */
static bool run_mp_overwrite_stress_test(void)
{
    struct ringbuf_mpsc *rb =
        ringbuf_mpsc_create(sizeof(struct message), 4, true);
    if (!rb) {
        fprintf(stderr, "mpsc_overwrite_stress: ringbuf_mpsc_create failed\n");
        return false;
    }
    bool ok = run_mp_stress_on(rb, 500000u, false, "mpsc_overwrite_stress");
    ringbuf_mpsc_destroy(rb);
    return ok;
}

#ifdef RINGBUF_TEST_HOOKS

/* ---- Counter-overflow tests.
 *
 * The internal counters are monotonic and only mapped onto slots via a mask,
 * so they eventually overflow SIZE_MAX. Reaching that organically would take
 * 2^64 pushes, so these tests park the counters just below the boundary with
 * ringbuf_mpsc_test_seed_counters() and then push across it.
 *
 * SEED_BASE leaves room for only 2 increments before write_idx wraps, so
 * every test below ends up with write_idx wrapped to a small value while
 * read_idx is still near SIZE_MAX. For this variant that also exercises the
 * stamps, which hold absolute ticket numbers and so wrap with the counters.
 * ---- */

#define SEED_BASE (SIZE_MAX - 2)

/**
 * run_wrap_boundary_test - fill, refuse, and drain across the SIZE_MAX
 *	counter boundary
 *
 * Sharp regression test: single-threaded, so a failure can only come from
 * the full/empty arithmetic rather than from a race.
 *
 * Return: true on pass.
 */
static bool run_wrap_boundary_test(void)
{
    const uint64_t capacity = 16;
    struct ringbuf_mpsc *rb =
        ringbuf_mpsc_create(sizeof(struct message), (size_t)capacity, false);
    if (!rb) {
        fprintf(stderr, "wrap_boundary: ringbuf_mpsc_create failed\n");
        return false;
    }
    ringbuf_mpsc_test_seed_counters(rb, SEED_BASE);

    if (!ringbuf_mpsc_is_empty_approx(rb)) {
        fprintf(stderr,
                "wrap_boundary: should read empty right after seeding\n");
        goto fail;
    }

    /* Fill to exactly capacity; write_idx crosses SIZE_MAX partway through. */
    for (uint64_t seq = 0; seq < capacity; seq++) {
        struct message msg;
        msg.seq = seq;
        msg.producer_id = 0;
        fill_payload(msg.payload, seq);
        if (!ringbuf_mpsc_push(rb, &msg)) {
            fprintf(stderr,
                    "wrap_boundary: push failed at seq %" PRIu64
                    " (expected room)\n",
                    seq);
            goto fail;
        }
    }

    if (!ringbuf_mpsc_is_full_approx(rb)) {
        fprintf(stderr,
                "wrap_boundary: should read full after %" PRIu64 " pushes\n",
                capacity);
        goto fail;
    }

    /* Non-overwrite buffer: the next push must be refused, not wrap around
     * and clobber unread data. */
    {
        struct message msg;
        msg.seq = capacity;
        msg.producer_id = 0;
        fill_payload(msg.payload, capacity);
        if (ringbuf_mpsc_push(rb, &msg)) {
            fprintf(stderr, "wrap_boundary: push succeeded on a full buffer\n");
            goto fail;
        }
    }

    /* write_idx has now wrapped past SIZE_MAX while read_idx has not. */
    for (uint64_t expected_seq = 0; expected_seq < capacity; expected_seq++) {
        struct message msg;
        if (!ringbuf_mpsc_pop(rb, &msg)) {
            fprintf(
                stderr,
                "wrap_boundary: pop reported empty across the SIZE_MAX boundary "
                "at seq %" PRIu64 "\n",
                expected_seq);
            goto fail;
        }
        if (msg.seq != expected_seq || !payload_ok(msg.payload, expected_seq)) {
            fprintf(stderr,
                    "wrap_boundary: mismatch, expected seq %" PRIu64
                    ", got %" PRIu64 "\n",
                    expected_seq, msg.seq);
            goto fail;
        }
    }

    {
        struct message msg;
        if (ringbuf_mpsc_pop(rb, &msg)) {
            fprintf(stderr,
                    "wrap_boundary: expected empty buffer after draining\n");
            goto fail;
        }
    }
    if (!ringbuf_mpsc_is_empty_approx(rb)) {
        fprintf(stderr, "wrap_boundary: should read empty after draining\n");
        goto fail;
    }

    ringbuf_mpsc_destroy(rb);
    printf("wrap_boundary: PASS (crossed SIZE_MAX, %" PRIu64 " messages)\n",
           capacity);
    return true;

fail:
    ringbuf_mpsc_destroy(rb);
    return false;
}

/**
 * run_wrap_overwrite_basic_test - eviction accounting across the same boundary
 *
 * Return: true on pass.
 */
static bool run_wrap_overwrite_basic_test(void)
{
    struct ringbuf_mpsc *rb =
        ringbuf_mpsc_create(sizeof(struct message), 4, true);
    if (!rb) {
        fprintf(stderr, "wrap_overwrite_basic: ringbuf_mpsc_create failed\n");
        return false;
    }
    ringbuf_mpsc_test_seed_counters(rb, SEED_BASE);
    bool ok = run_overwrite_basic_on(rb, "wrap_overwrite_basic");
    ringbuf_mpsc_destroy(rb);
    return ok;
}

/**
 * run_wrap_lossless_concurrent_test - concurrent lossless coverage of the
 *	boundary, for ThreadSanitizer
 *
 * Smaller counts than the main stress tests: the boundary is crossed in the
 * first handful of pushes, so the rest is just ordinary running.
 *
 * Return: true on pass.
 */
static bool run_wrap_lossless_concurrent_test(void)
{
    struct ringbuf_mpsc *rb =
        ringbuf_mpsc_create(sizeof(struct message), 16, false);
    if (!rb) {
        fprintf(stderr,
                "wrap_lossless_concurrent: ringbuf_mpsc_create failed\n");
        return false;
    }
    ringbuf_mpsc_test_seed_counters(rb, SEED_BASE);
    bool ok = run_lossless_transfer(rb, 200000u, "wrap_lossless_concurrent");
    ringbuf_mpsc_destroy(rb);
    return ok;
}

/**
 * run_wrap_overwrite_stress_test - concurrent eviction coverage of the boundary
 *
 * Return: true on pass.
 */
static bool run_wrap_overwrite_stress_test(void)
{
    struct ringbuf_mpsc *rb =
        ringbuf_mpsc_create(sizeof(struct message), 4, true);
    if (!rb) {
        fprintf(stderr, "wrap_overwrite_stress: ringbuf_mpsc_create failed\n");
        return false;
    }
    ringbuf_mpsc_test_seed_counters(rb, SEED_BASE);
    bool ok = run_overwrite_stress_on(rb, 200000u, "wrap_overwrite_stress");
    ringbuf_mpsc_destroy(rb);
    return ok;
}

/**
 * run_mp_wrap_overwrite_stress_test - multi-producer eviction across the
 *	SIZE_MAX boundary
 *
 * The combination the other wrap cases do not reach: several producers
 * contending on the stamp CAS while the absolute ticket numbers those
 * stamps hold wrap past SIZE_MAX.
 *
 * Return: true on pass.
 */
static bool run_mp_wrap_overwrite_stress_test(void)
{
    struct ringbuf_mpsc *rb =
        ringbuf_mpsc_create(sizeof(struct message), 4, true);
    if (!rb) {
        fprintf(stderr,
                "mpsc_wrap_overwrite_stress: ringbuf_mpsc_create failed\n");
        return false;
    }
    ringbuf_mpsc_test_seed_counters(rb, SEED_BASE);
    bool ok =
        run_mp_stress_on(rb, 200000u, false, "mpsc_wrap_overwrite_stress");
    ringbuf_mpsc_destroy(rb);
    return ok;
}

#endif /* RINGBUF_TEST_HOOKS */

/**
 * main - run all ten cases unconditionally
 *
 * There is no filter flag; the five counter-overflow cases are present only
 * under -DRINGBUF_TEST_HOOKS.
 *
 * Return: 0 if every case passed, 1 otherwise.
 */
int main(void)
{
    bool ok = true;
    ok &= run_lossless_test();
    ok &= run_overwrite_basic_test();
    ok &= run_overwrite_stress_test();
    ok &= run_mp_lossless_test();
    ok &= run_mp_overwrite_stress_test();
#ifdef RINGBUF_TEST_HOOKS
    ok &= run_wrap_boundary_test();
    ok &= run_wrap_overwrite_basic_test();
    ok &= run_wrap_lossless_concurrent_test();
    ok &= run_wrap_overwrite_stress_test();
    ok &= run_mp_wrap_overwrite_stress_test();
#else
    printf(
        "counter-overflow tests SKIPPED (build with -DRINGBUF_TEST_HOOKS)\n");
#endif

    if (!ok) {
        printf("FAIL\n");
        return 1;
    }

    printf("PASS\n");
    return 0;
}
