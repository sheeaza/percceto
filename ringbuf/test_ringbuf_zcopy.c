/*
 * Tests for the reserve/submit variant.
 *
 * The first seven cases mirror test_ringbuf_mutex.c case for case, driven
 * through ringbuf_zcopy_push(), so the two can be compared directly. That is
 * not busywork: push() leaves no reservation outstanding, so those cases
 * assert the header's equivalence claim -- used only through push(), this
 * variant behaves exactly like ringbuf_mutex, including overwrite mode's
 * promise that push NEVER fails. Hence the overwrite producers below have no
 * retry loop, as in test_ringbuf_mutex.c.
 *
 * The remaining seven exercise what is actually new: many outstanding
 * reservations, submitted out of order, filled from other threads.
 *
 * One rule applies throughout and differs from every other test file here:
 * a false return from ringbuf_zcopy_pop() does NOT mean empty, because a
 * reserved-but-unsubmitted slot blocks delivery head-of-line. Consumers must
 * retry, and may only conclude "drained" once the producer side is done.
 */

#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "ringbuf_zcopy.h"

#define PAYLOAD_LEN 24

struct message {
    uint64_t seq;
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

/**
 * fill_in_place - write a whole message directly into a reserved slot
 * @slot: a reservation's data pointer, slot_size >= sizeof(struct message)
 * @seq: sequence number to stamp the message with
 *
 * The zero-copy write the variant exists for: the message is assembled in
 * the ring itself, with no staging buffer and no second copy.
 */
static void fill_in_place(void *slot, uint64_t seq)
{
    struct message *m = slot;
    m->seq = seq;
    fill_payload(m->payload, seq);
}

/* Upper bound on consecutive failed reserve/pop attempts before a test
 * declares a stall. Legitimate contention resolves within a few thousand
 * yields even when the peer thread loses its timeslice, so this is a large
 * safety margin -- but it is bounded, so a regression that wedges the buffer
 * (e.g. a slot stuck reading as unsubmitted forever) fails the suite instead
 * of hanging it. */
#define MAX_SPIN 2000000u

/* ---- Lossless transfer (overwrite_on_full = false) ---- */

struct lossless_producer_args {
    struct ringbuf_zcopy *rb;
    uint64_t num_messages;
    bool failed;
    const char *label;
};

struct lossless_consumer_args {
    struct ringbuf_zcopy *rb;
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
        fill_payload(msg.payload, seq);

        /* Lossless mode: push legitimately fails on a full buffer, so this
         * retry loop stays. */
        uint64_t spins = 0;
        while (!ringbuf_zcopy_push(a->rb, &msg)) {
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
        while (!ringbuf_zcopy_pop(a->rb, &msg)) {
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
static bool run_lossless_transfer(struct ringbuf_zcopy *rb, uint64_t n,
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
    struct ringbuf_zcopy *rb =
        ringbuf_zcopy_create(sizeof(struct message), 16, false);
    if (!rb) {
        fprintf(stderr, "lossless: ringbuf_zcopy_create failed\n");
        return false;
    }
    bool ok = run_lossless_transfer(rb, 2000000u, "lossless");
    ringbuf_zcopy_destroy(rb);
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
static bool run_overwrite_basic_on(struct ringbuf_zcopy *rb, const char *label)
{
    for (uint64_t seq = 0; seq < 10; seq++) {
        struct message msg;
        msg.seq = seq;
        fill_payload(msg.payload, seq);
        if (!ringbuf_zcopy_push(rb, &msg)) {
            fprintf(stderr, "%s: push unexpectedly failed at seq %" PRIu64 "\n",
                    label, seq);
            return false;
        }
    }

    size_t dropped = ringbuf_zcopy_dropped_count(rb);
    if (dropped != 6) {
        fprintf(stderr, "%s: expected 6 dropped, got %zu\n", label, dropped);
        return false;
    }

    for (uint64_t expected_seq = 6; expected_seq < 10; expected_seq++) {
        struct message msg;
        if (!ringbuf_zcopy_pop(rb, &msg)) {
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
    if (ringbuf_zcopy_pop(rb, &msg)) {
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
    struct ringbuf_zcopy *rb =
        ringbuf_zcopy_create(sizeof(struct message), 4, true);
    if (!rb) {
        fprintf(stderr, "overwrite_basic: ringbuf_zcopy_create failed\n");
        return false;
    }
    bool ok = run_overwrite_basic_on(rb, "overwrite_basic");
    ringbuf_zcopy_destroy(rb);
    return ok;
}

/* ---- Overwrite mode, concurrent stress. A tiny capacity maximizes
 * eviction-vs-read contention on the shared boundary slot. ---- */

struct stress_producer_args {
    struct ringbuf_zcopy *rb;
    uint64_t num_messages;
    atomic_bool *done;
    bool failed;
    const char *label;
};

struct stress_consumer_args {
    struct ringbuf_zcopy *rb;
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
 * Has no retry loop on purpose: push() leaves no reservation outstanding, so
 * in overwrite mode it inherits ringbuf_mutex's promise that it never fails,
 * and a false return is a hard failure. Sets *done once finished, so the
 * consumer knows when to drain and stop.
 *
 * Return: NULL always; failure is reported via the args' @failed field.
 */
static void *stress_producer_main(void *arg)
{
    struct stress_producer_args *a = arg;
    for (uint64_t seq = 0; seq < a->num_messages; seq++) {
        struct message msg;
        msg.seq = seq;
        fill_payload(msg.payload, seq);

        /* The contract under test: through push(), overwrite mode always
         * succeeds. No retry loop -- it blocks on the lock instead of
         * failing, so a false return here is a real regression. This is the
         * one case that exercises it under heavy contention, with capacity 4
         * and millions of messages. */
        if (!ringbuf_zcopy_push(a->rb, &msg)) {
            fprintf(stderr,
                    "%s: push returned false in overwrite mode at seq %" PRIu64
                    " (must never happen)\n",
                    a->label, seq);
            a->failed = true;
            atomic_store_explicit(a->done, true, memory_order_release);
            return NULL;
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
        if (!ringbuf_zcopy_pop(a->rb, &msg)) {
            if (atomic_load_explicit(a->done, memory_order_acquire)) {
                /* Safe to conclude "drained" only here: the producer is
                 * finished, so no reservation is outstanding and a false
                 * return really does mean empty. */
                if (!ringbuf_zcopy_pop(a->rb, &msg))
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
 * Return: true if push never failed, payloads were intact, seqs strictly
 * increasing, the final message observed, and received + dropped == @n.
 */
static bool run_overwrite_stress_on(struct ringbuf_zcopy *rb, uint64_t n,
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

    size_t dropped = ringbuf_zcopy_dropped_count(rb);

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
    /* The accounting invariant: every message is either received or dropped,
     * never both and never neither. This is what catches a pop that acted on
     * a read index sampled before it took the lock. */
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
    struct ringbuf_zcopy *rb =
        ringbuf_zcopy_create(sizeof(struct message), 4, true);
    if (!rb) {
        fprintf(stderr, "overwrite_stress: ringbuf_zcopy_create failed\n");
        return false;
    }
    bool ok = run_overwrite_stress_on(rb, 2000000u, "overwrite_stress");
    ringbuf_zcopy_destroy(rb);
    return ok;
}

/* ---- Reserve/submit: the cases specific to this variant ---- */

/**
 * run_reserve_basic_test - reserve, fill in place, submit, pop
 *
 * The minimal end-to-end exercise of the split push, single-threaded, with
 * nothing concurrent that could mask a mistake.
 *
 * Return: true on pass.
 */
static bool run_reserve_basic_test(void)
{
    struct ringbuf_zcopy *rb =
        ringbuf_zcopy_create(sizeof(struct message), 8, false);
    if (!rb) {
        fprintf(stderr, "reserve_basic: ringbuf_zcopy_create failed\n");
        return false;
    }

    bool ok = false;

    for (uint64_t seq = 0; seq < 100; seq++) {
        struct ringbuf_rsv rsv;
        struct message msg;

        if (!ringbuf_zcopy_reserve(rb, &rsv)) {
            fprintf(stderr,
                    "reserve_basic: reserve failed at seq %" PRIu64 "\n", seq);
            goto out;
        }

        /* Nothing is delivered until submit, even though the bytes are
         * already in the ring. */
        fill_in_place(rsv.data, seq);
        if (ringbuf_zcopy_pop(rb, &msg)) {
            fprintf(stderr,
                    "reserve_basic: pop succeeded before submit at seq %" PRIu64
                    "\n",
                    seq);
            goto out;
        }

        ringbuf_zcopy_submit(rb, &rsv);

        if (!ringbuf_zcopy_pop(rb, &msg)) {
            fprintf(stderr,
                    "reserve_basic: pop failed after submit at seq %" PRIu64
                    "\n",
                    seq);
            goto out;
        }
        if (msg.seq != seq || !payload_ok(msg.payload, seq)) {
            fprintf(stderr,
                    "reserve_basic: mismatch, expected seq %" PRIu64
                    ", got %" PRIu64 "\n",
                    seq, msg.seq);
            goto out;
        }
    }

    ok = true;
    printf("reserve_basic: PASS (100 messages)\n");

out:
    ringbuf_zcopy_destroy(rb);
    return ok;
}

/**
 * run_ooo_submit_test - submit three reservations in reverse order
 *
 * The core new guarantee, and the head-of-line rule that comes with it:
 * delivery follows RESERVATION order regardless of submit order, so
 * submitting c first does not deliver c first. While reservation a is
 * outstanding, pop reports false even though is_empty_approx reports
 * non-empty -- the one place those two disagree, and the reason the header
 * insists false does not mean empty.
 *
 * Return: true on pass.
 */
static bool run_ooo_submit_test(void)
{
    struct ringbuf_zcopy *rb =
        ringbuf_zcopy_create(sizeof(struct message), 4, false);
    if (!rb) {
        fprintf(stderr, "ooo_submit: ringbuf_zcopy_create failed\n");
        return false;
    }

    bool ok = false;
    struct ringbuf_rsv a, b, c;
    struct message msg;

    if (!ringbuf_zcopy_reserve(rb, &a) || !ringbuf_zcopy_reserve(rb, &b) ||
        !ringbuf_zcopy_reserve(rb, &c)) {
        fprintf(stderr, "ooo_submit: reserve failed with room to spare\n");
        goto out;
    }

    /* All three fills happen before any submit, which is what a parallel
     * filler pool does; here they are sequential so the ordering checks
     * below are deterministic. */
    fill_in_place(a.data, 10);
    fill_in_place(b.data, 11);
    fill_in_place(c.data, 12);

    /* Three distinct reservations must be three distinct slots. */
    if (a.data == b.data || b.data == c.data || a.data == c.data) {
        fprintf(stderr, "ooo_submit: reservations aliased the same slot\n");
        goto out;
    }

    ringbuf_zcopy_submit(rb, &c);

    /* c is submitted, but a and b are not, so nothing may be delivered. */
    if (ringbuf_zcopy_pop(rb, &msg)) {
        fprintf(stderr,
                "ooo_submit: pop delivered seq %" PRIu64
                " out of reservation order\n",
                msg.seq);
        goto out;
    }
    if (ringbuf_zcopy_is_empty_approx(rb)) {
        fprintf(stderr, "ooo_submit: is_empty_approx should count outstanding "
                        "reservations as non-empty\n");
        goto out;
    }

    ringbuf_zcopy_submit(rb, &a);

    /* a is now the oldest and submitted, so it is delivered -- then b blocks
     * c again, even though c has been submitted all along. */
    if (!ringbuf_zcopy_pop(rb, &msg) || msg.seq != 10 ||
        !payload_ok(msg.payload, 10)) {
        fprintf(stderr, "ooo_submit: expected seq 10 after submitting a\n");
        goto out;
    }
    if (ringbuf_zcopy_pop(rb, &msg)) {
        fprintf(stderr,
                "ooo_submit: pop delivered seq %" PRIu64
                " while b was unsubmitted\n",
                msg.seq);
        goto out;
    }

    ringbuf_zcopy_submit(rb, &b);

    if (!ringbuf_zcopy_pop(rb, &msg) || msg.seq != 11 ||
        !payload_ok(msg.payload, 11)) {
        fprintf(stderr, "ooo_submit: expected seq 11 after submitting b\n");
        goto out;
    }
    if (!ringbuf_zcopy_pop(rb, &msg) || msg.seq != 12 ||
        !payload_ok(msg.payload, 12)) {
        fprintf(stderr, "ooo_submit: expected seq 12 to follow\n");
        goto out;
    }
    if (ringbuf_zcopy_pop(rb, &msg)) {
        fprintf(stderr, "ooo_submit: expected empty buffer after draining\n");
        goto out;
    }

    ok = true;
    printf("ooo_submit: PASS (submitted c,a,b -- delivered a,b,c)\n");

out:
    ringbuf_zcopy_destroy(rb);
    return ok;
}

/**
 * run_reserve_exhaust_test - fill the ring with reservations, lossless mode
 *
 * Pins down both of reserve's false returns and the difference between them.
 * With capacity reservations outstanding the next one fails; submitting one
 * is NOT enough to free a slot, because the message then exists and is
 * unread -- only a pop frees it. A regression that conflated "unsubmitted"
 * with "free" would let the reserve after the submit succeed.
 *
 * Return: true on pass.
 */
static bool run_reserve_exhaust_test(void)
{
    struct ringbuf_zcopy *rb =
        ringbuf_zcopy_create(sizeof(struct message), 4, false);
    if (!rb) {
        fprintf(stderr, "reserve_exhaust: ringbuf_zcopy_create failed\n");
        return false;
    }

    bool ok = false;
    size_t capacity = ringbuf_zcopy_capacity(rb);
    struct ringbuf_rsv rsv[4];
    struct ringbuf_rsv extra;
    struct message msg;

    if (capacity != 4) {
        fprintf(stderr, "reserve_exhaust: expected capacity 4, got %zu\n",
                capacity);
        goto out;
    }

    for (size_t i = 0; i < capacity; i++) {
        if (!ringbuf_zcopy_reserve(rb, &rsv[i])) {
            fprintf(stderr, "reserve_exhaust: reserve %zu failed early\n", i);
            goto out;
        }
        fill_in_place(rsv[i].data, (uint64_t)i);
    }

    if (!ringbuf_zcopy_is_full_approx(rb)) {
        fprintf(stderr,
                "reserve_exhaust: should read full with %zu reservations "
                "outstanding\n",
                capacity);
        goto out;
    }

    /* Every slot is reserved, so there is nothing to hand out. */
    if (ringbuf_zcopy_reserve(rb, &extra)) {
        fprintf(stderr, "reserve_exhaust: reserve succeeded on a full ring\n");
        goto out;
    }

    ringbuf_zcopy_submit(rb, &rsv[0]);

    /* Still full: the slot now holds a real unread message rather than an
     * in-progress fill, which is lossless mode's other reason to refuse. */
    if (ringbuf_zcopy_reserve(rb, &extra)) {
        fprintf(stderr,
                "reserve_exhaust: reserve succeeded after submit alone -- a "
                "submitted message is not a free slot\n");
        goto out;
    }

    if (!ringbuf_zcopy_pop(rb, &msg) || msg.seq != 0) {
        fprintf(stderr, "reserve_exhaust: expected seq 0 after submit\n");
        goto out;
    }

    /* The pop freed exactly one slot. */
    if (!ringbuf_zcopy_reserve(rb, &extra)) {
        fprintf(stderr, "reserve_exhaust: reserve failed after a pop freed a "
                        "slot\n");
        goto out;
    }
    fill_in_place(extra.data, 4);
    ringbuf_zcopy_submit(rb, &extra);

    /* Drain, submitting the rest in an order unrelated to reservation
     * order. Delivery must still be 1, 2, 3, 4. */
    ringbuf_zcopy_submit(rb, &rsv[2]);
    ringbuf_zcopy_submit(rb, &rsv[1]);
    ringbuf_zcopy_submit(rb, &rsv[3]);

    for (uint64_t expected_seq = 1; expected_seq <= 4; expected_seq++) {
        if (!ringbuf_zcopy_pop(rb, &msg) || msg.seq != expected_seq ||
            !payload_ok(msg.payload, expected_seq)) {
            fprintf(stderr,
                    "reserve_exhaust: expected seq %" PRIu64
                    " while draining\n",
                    expected_seq);
            goto out;
        }
    }
    if (ringbuf_zcopy_pop(rb, &msg)) {
        fprintf(stderr,
                "reserve_exhaust: expected empty buffer after draining\n");
        goto out;
    }

    ok = true;
    printf("reserve_exhaust: PASS (capacity=%zu)\n", capacity);

out:
    ringbuf_zcopy_destroy(rb);
    return ok;
}

/**
 * run_unsubmitted_blocks_evict_test - an unsubmitted slot is never evicted
 *
 * The sharpest single check in this file. In overwrite mode a full ring
 * normally evicts, and push therefore never fails -- but a slot whose fill
 * is still in progress must NOT be evicted, since a filler thread may be
 * writing to it. reserve has to refuse instead, and recover once the slot is
 * submitted.
 *
 * This is also what the eviction branch's re-stamp protects across laps:
 * treating "reserved but unsubmitted" as evictable hands the same bytes to a
 * filler and an evictor at once.
 *
 * Return: true on pass.
 */
static bool run_unsubmitted_blocks_evict_test(void)
{
    struct ringbuf_zcopy *rb =
        ringbuf_zcopy_create(sizeof(struct message), 4, true);
    if (!rb) {
        fprintf(stderr,
                "unsubmitted_blocks_evict: ringbuf_zcopy_create failed\n");
        return false;
    }

    bool ok = false;
    struct ringbuf_rsv held, next;
    struct message msg;
    size_t dropped;

    /* Reserve slot 0 and deliberately leave it unsubmitted, as if a filler
     * thread were still working on it. */
    if (!ringbuf_zcopy_reserve(rb, &held)) {
        fprintf(stderr, "unsubmitted_blocks_evict: first reserve failed\n");
        goto out;
    }
    fill_in_place(held.data, 0);

    /* Fill the other three slots the ordinary way. */
    for (uint64_t seq = 1; seq < 4; seq++) {
        struct message tmp;
        tmp.seq = seq;
        fill_payload(tmp.payload, seq);
        if (!ringbuf_zcopy_push(rb, &tmp)) {
            fprintf(stderr,
                    "unsubmitted_blocks_evict: push failed at seq %" PRIu64
                    " with room\n",
                    seq);
            goto out;
        }
    }

    /* The ring is full and the oldest slot is mid-fill. Overwrite mode must
     * refuse rather than evict it -- the one case where push can fail in
     * this mode, and only because the caller is holding a reservation. */
    if (ringbuf_zcopy_reserve(rb, &next)) {
        fprintf(stderr, "unsubmitted_blocks_evict: evicted a slot that was "
                        "still being filled\n");
        goto out;
    }
    if (ringbuf_zcopy_dropped_count(rb) != 0) {
        fprintf(stderr,
                "unsubmitted_blocks_evict: counted a drop for a refused "
                "reserve (dropped=%zu)\n",
                ringbuf_zcopy_dropped_count(rb));
        goto out;
    }

    /* The fill completes. The slot now holds an unread message, which IS
     * evictable, so the same reserve must now succeed and count a drop. */
    ringbuf_zcopy_submit(rb, &held);

    if (!ringbuf_zcopy_reserve(rb, &next)) {
        fprintf(stderr, "unsubmitted_blocks_evict: reserve still refused after "
                        "the fill was submitted\n");
        goto out;
    }
    fill_in_place(next.data, 4);
    ringbuf_zcopy_submit(rb, &next);

    dropped = ringbuf_zcopy_dropped_count(rb);
    if (dropped != 1) {
        fprintf(stderr,
                "unsubmitted_blocks_evict: expected 1 dropped, got %zu\n",
                dropped);
        goto out;
    }

    /* seq 0 was the eviction victim, so 1..4 survive. */
    for (uint64_t expected_seq = 1; expected_seq <= 4; expected_seq++) {
        if (!ringbuf_zcopy_pop(rb, &msg) || msg.seq != expected_seq ||
            !payload_ok(msg.payload, expected_seq)) {
            fprintf(stderr,
                    "unsubmitted_blocks_evict: expected seq %" PRIu64 "\n",
                    expected_seq);
            goto out;
        }
    }
    if (ringbuf_zcopy_pop(rb, &msg)) {
        fprintf(stderr,
                "unsubmitted_blocks_evict: expected empty after draining\n");
        goto out;
    }

    ok = true;
    printf("unsubmitted_blocks_evict: PASS (dropped=%zu)\n", dropped);

out:
    ringbuf_zcopy_destroy(rb);
    return ok;
}

/* ---- Parallel fill: the point of the variant, and the real gate on the
 * three-edge ordering described in ringbuf_zcopy_reserve.
 *
 * A reserving thread hands reservations to a pool of filler threads through a
 * tiny lock-free job array. Each filler writes the message directly into the
 * ring and submits it itself, so submits happen on several threads at once
 * and in an order decided by the scheduler. ---- */

#define NUM_FILLERS 4
#define NUM_JOBS 8

#define JOB_EMPTY 0
#define JOB_READY 1
#define JOB_CLAIMED 2

struct job {
    /* JOB_EMPTY -> JOB_READY (reserver) -> JOB_CLAIMED (one filler) ->
     * JOB_EMPTY. The release/acquire pair on this is edge 2 of the three:
     * the caller-supplied handoff that publishes the reservation to whoever
     * fills it. */
    atomic_int state;
    struct ringbuf_rsv rsv;
    uint64_t seq;
};

struct fill_pool {
    struct job jobs[NUM_JOBS];
    atomic_bool reserving_done;
};

struct filler_args {
    struct ringbuf_zcopy *rb;
    struct fill_pool *pool;
    uint64_t filled;
};

struct reserver_args {
    struct ringbuf_zcopy *rb;
    struct fill_pool *pool;
    uint64_t num_messages;
    bool overwrite;
    bool failed;
    const char *label;
};

/**
 * filler_main - fill and submit reservations posted by the reserver
 * @arg: struct filler_args
 *
 * Runs on each of NUM_FILLERS threads. Writes the message straight into the
 * ring and calls submit itself, which is what the variant exists to allow.
 * Exits once the reserver is done and no job is left outstanding; the
 * reserving_done load is sampled BEFORE the scan, so a job posted
 * concurrently with it is still seen by the scan that follows.
 *
 * Return: NULL always; the per-thread fill count lands in the args.
 */
static void *filler_main(void *arg)
{
    struct filler_args *a = arg;

    for (;;) {
        bool done = atomic_load_explicit(&a->pool->reserving_done,
                                         memory_order_acquire);
        bool worked = false;

        for (size_t i = 0; i < NUM_JOBS; i++) {
            struct job *j = &a->pool->jobs[i];

            int expected = JOB_READY;
            /* Acquire on success: the reservation handle and seq written by
             * the reserver must be visible before we touch rsv.data. */
            if (!atomic_compare_exchange_strong_explicit(
                    &j->state, &expected, JOB_CLAIMED, memory_order_acquire,
                    memory_order_relaxed))
                continue;

            fill_in_place(j->rsv.data, j->seq);
            ringbuf_zcopy_submit(a->rb, &j->rsv);
            a->filled++;
            worked = true;

            atomic_store_explicit(&j->state, JOB_EMPTY, memory_order_release);
        }

        if (done && !worked)
            return NULL;
        if (!worked)
            sched_yield();
    }
}

/**
 * post_job - hand one reservation to the filler pool
 * @pool: the job array
 * @rsv: reservation to fill
 * @seq: sequence number the filler should stamp it with
 *
 * Spins for a free job slot, which is bounded by the pool draining -- the
 * fillers never block on anything but their own CAS.
 *
 * Return: true if the job was posted; false if no filler took a job within
 * MAX_SPIN scans, which means the pool has stalled.
 */
static bool post_job(struct fill_pool *pool, const struct ringbuf_rsv *rsv,
                     uint64_t seq)
{
    uint64_t spins = 0;

    for (;;) {
        for (size_t i = 0; i < NUM_JOBS; i++) {
            struct job *j = &pool->jobs[i];

            /* Acquire, not relaxed, and for the mirror image of the reason
             * the store below is release: the filler release-stores
             * JOB_EMPTY once it has finished reading j->rsv, and without
             * this acquire our writes to j->rsv below would race that read.
             * Recycling a job slot needs an edge in both directions. */
            if (atomic_load_explicit(&j->state, memory_order_acquire) !=
                JOB_EMPTY)
                continue;

            j->rsv = *rsv;
            j->seq = seq;
            /* Release: publishes the two writes above to the filler's
             * acquiring CAS. */
            atomic_store_explicit(&j->state, JOB_READY, memory_order_release);
            return true;
        }

        if (++spins > MAX_SPIN)
            return false;
        sched_yield();
    }
}

/**
 * reserver_main - reserve slots and hand each to the filler pool
 * @arg: struct reserver_args
 *
 * The single producer-side thread: it only ever reserves, never writes a
 * payload. Sets pool->reserving_done when finished, which lets the fillers
 * exit.
 *
 * Return: NULL always; failure is reported via the args' @failed field.
 */
static void *reserver_main(void *arg)
{
    struct reserver_args *a = arg;

    for (uint64_t seq = 0; seq < a->num_messages; seq++) {
        struct ringbuf_rsv rsv;

        /* Retry in both modes. Lossless mode fails when the ring is full of
         * unread messages; overwrite mode fails only while the slot it wants
         * is still being filled, which a filler clears shortly. */
        uint64_t spins = 0;
        while (!ringbuf_zcopy_reserve(a->rb, &rsv)) {
            if (++spins > MAX_SPIN) {
                fprintf(stderr, "%s: reserver stalled at seq %" PRIu64 "\n",
                        a->label, seq);
                a->failed = true;
                goto done;
            }
            sched_yield();
        }

        if (!post_job(a->pool, &rsv, seq)) {
            fprintf(stderr, "%s: filler pool stalled at seq %" PRIu64 "\n",
                    a->label, seq);
            a->failed = true;
            goto done;
        }
    }

done:
    atomic_store_explicit(&a->pool->reserving_done, true, memory_order_release);
    return NULL;
}

/**
 * run_parallel_fill_on - reserve on one thread, fill and submit on several
 * @rb: the buffer
 * @n: number of messages to send
 * @overwrite: true if @rb was created in overwrite mode
 * @label: name used in progress and failure messages
 *
 * In lossless mode every message must arrive, in order. In overwrite mode
 * gaps are expected but received + dropped == @n must still hold, and no
 * payload may be torn -- which is what a missing release on submit, or an
 * eviction of a slot still being filled, would produce.
 *
 * Return: true on pass.
 */
static bool run_parallel_fill_on(struct ringbuf_zcopy *rb, uint64_t n,
                                 bool overwrite, const char *label)
{
    struct fill_pool pool;
    for (size_t i = 0; i < NUM_JOBS; i++) {
        atomic_init(&pool.jobs[i].state, JOB_EMPTY);
        pool.jobs[i].seq = 0;
    }
    atomic_init(&pool.reserving_done, false);

    struct reserver_args rargs = { .rb = rb,
                                   .pool = &pool,
                                   .num_messages = n,
                                   .overwrite = overwrite,
                                   .failed = false,
                                   .label = label };
    struct filler_args fargs[NUM_FILLERS];
    for (size_t i = 0; i < NUM_FILLERS; i++)
        fargs[i] = (struct filler_args){ .rb = rb, .pool = &pool, .filled = 0 };

    /* `done` for the consumer is set only after every filler has joined, so
     * at that point no reservation is outstanding and a false return from pop
     * really does mean empty. */
    atomic_bool done = false;
    struct stress_consumer_args cargs = { .rb = rb,
                                          .done = &done,
                                          .received = 0,
                                          .last_seq = 0,
                                          .corrupted = false,
                                          .out_of_order = false,
                                          .stalled = false,
                                          .label = label };

    pthread_t consumer, reserver, fillers[NUM_FILLERS];
    pthread_create(&consumer, NULL, stress_consumer_main, &cargs);
    pthread_create(&reserver, NULL, reserver_main, &rargs);
    for (size_t i = 0; i < NUM_FILLERS; i++)
        pthread_create(&fillers[i], NULL, filler_main, &fargs[i]);

    pthread_join(reserver, NULL);
    for (size_t i = 0; i < NUM_FILLERS; i++)
        pthread_join(fillers[i], NULL);
    atomic_store_explicit(&done, true, memory_order_release);
    pthread_join(consumer, NULL);

    size_t dropped = ringbuf_zcopy_dropped_count(rb);
    uint64_t filled = 0;
    for (size_t i = 0; i < NUM_FILLERS; i++)
        filled += fargs[i].filled;

    if (rargs.failed || cargs.stalled)
        return false;
    if (cargs.corrupted) {
        fprintf(stderr, "%s: payload corruption detected\n", label);
        return false;
    }
    if (cargs.out_of_order) {
        fprintf(stderr, "%s: out-of-order/duplicate seq detected\n", label);
        return false;
    }
    /* Every reservation was submitted exactly once, by exactly one filler. */
    if (filled != n) {
        fprintf(stderr,
                "%s: fillers submitted %" PRIu64 " of %" PRIu64 " messages\n",
                label, filled, n);
        return false;
    }
    if (cargs.received + dropped != n) {
        fprintf(stderr,
                "%s: received(%" PRIu64 ") + dropped(%zu) != pushed(%" PRIu64
                ")\n",
                label, cargs.received, dropped, n);
        return false;
    }
    if (!overwrite && cargs.received != n) {
        fprintf(stderr,
                "%s: lossless mode lost messages (received=%" PRIu64
                " of %" PRIu64 ")\n",
                label, cargs.received, n);
        return false;
    }
    /* The last reservation cannot be evicted -- nothing is pushed after it --
     * so it must be the last message delivered, in either mode. */
    if (cargs.last_seq != n - 1) {
        fprintf(stderr,
                "%s: did not observe final message (last_seq=%" PRIu64 ")\n",
                label, cargs.last_seq);
        return false;
    }

    printf("%s: PASS (received=%" PRIu64 ", dropped=%zu, %d fillers)\n", label,
           cargs.received, dropped, NUM_FILLERS);
    return true;
}

/**
 * run_parallel_fill_test - parallel fill in lossless mode
 *
 * Return: true on pass.
 */
static bool run_parallel_fill_test(void)
{
    struct ringbuf_zcopy *rb =
        ringbuf_zcopy_create(sizeof(struct message), 16, false);
    if (!rb) {
        fprintf(stderr, "parallel_fill: ringbuf_zcopy_create failed\n");
        return false;
    }
    bool ok = run_parallel_fill_on(rb, 200000u, false, "parallel_fill");
    ringbuf_zcopy_destroy(rb);
    return ok;
}

/**
 * run_parallel_fill_overwrite_test - parallel fill under eviction
 *
 * Capacity 4 with four fillers keeps the ring saturated, so evictions race
 * in-flight fills continuously -- the configuration that would expose an
 * eviction of a slot still being written.
 *
 * Return: true on pass.
 */
static bool run_parallel_fill_overwrite_test(void)
{
    struct ringbuf_zcopy *rb =
        ringbuf_zcopy_create(sizeof(struct message), 4, true);
    if (!rb) {
        fprintf(stderr,
                "parallel_fill_overwrite: ringbuf_zcopy_create failed\n");
        return false;
    }
    bool ok =
        run_parallel_fill_on(rb, 200000u, true, "parallel_fill_overwrite");
    ringbuf_zcopy_destroy(rb);
    return ok;
}

#ifdef RINGBUF_TEST_HOOKS

/* ---- Counter-overflow tests.
 *
 * The internal counters are monotonic and only mapped onto slots via a mask,
 * so they eventually overflow SIZE_MAX. Reaching that organically would take
 * 2^64 reservations, so these tests park the counters just below the boundary
 * with ringbuf_zcopy_test_seed_counters() and then push across it.
 *
 * SEED_BASE leaves room for only 2 increments before write_idx wraps, so
 * every test below ends up with write_idx wrapped to a small value while
 * read_idx is still near SIZE_MAX. The stamps hold absolute ticket numbers
 * and so cross the boundary too, which is why the seed hook has to re-stamp
 * every slot -- without that, these cases hang rather than fail.
 * ---- */

#define SEED_BASE (SIZE_MAX - 2)

/**
 * run_wrap_boundary_test - fill, refuse, and drain across the SIZE_MAX
 *	counter boundary
 *
 * Sharp regression test: single-threaded, so a failure can only come from
 * the full/empty arithmetic or the stamp encoding rather than from a race.
 *
 * Return: true on pass.
 */
static bool run_wrap_boundary_test(void)
{
    const uint64_t capacity = 16;
    struct ringbuf_zcopy *rb =
        ringbuf_zcopy_create(sizeof(struct message), (size_t)capacity, false);
    if (!rb) {
        fprintf(stderr, "wrap_boundary: ringbuf_zcopy_create failed\n");
        return false;
    }
    ringbuf_zcopy_test_seed_counters(rb, SEED_BASE);

    if (!ringbuf_zcopy_is_empty_approx(rb)) {
        fprintf(stderr,
                "wrap_boundary: should read empty right after seeding\n");
        goto fail;
    }

    /* Fill to exactly capacity; write_idx crosses SIZE_MAX partway through. */
    for (uint64_t seq = 0; seq < capacity; seq++) {
        struct message msg;
        msg.seq = seq;
        fill_payload(msg.payload, seq);
        if (!ringbuf_zcopy_push(rb, &msg)) {
            fprintf(stderr,
                    "wrap_boundary: push failed at seq %" PRIu64
                    " (expected room)\n",
                    seq);
            goto fail;
        }
    }

    if (!ringbuf_zcopy_is_full_approx(rb)) {
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
        fill_payload(msg.payload, capacity);
        if (ringbuf_zcopy_push(rb, &msg)) {
            fprintf(stderr, "wrap_boundary: push succeeded on a full buffer\n");
            goto fail;
        }
    }

    /* write_idx has now wrapped past SIZE_MAX while read_idx has not. */
    for (uint64_t expected_seq = 0; expected_seq < capacity; expected_seq++) {
        struct message msg;
        if (!ringbuf_zcopy_pop(rb, &msg)) {
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
        if (ringbuf_zcopy_pop(rb, &msg)) {
            fprintf(stderr,
                    "wrap_boundary: expected empty buffer after draining\n");
            goto fail;
        }
    }
    if (!ringbuf_zcopy_is_empty_approx(rb)) {
        fprintf(stderr, "wrap_boundary: should read empty after draining\n");
        goto fail;
    }

    ringbuf_zcopy_destroy(rb);
    printf("wrap_boundary: PASS (crossed SIZE_MAX, %" PRIu64 " messages)\n",
           capacity);
    return true;

fail:
    ringbuf_zcopy_destroy(rb);
    return false;
}

/**
 * run_wrap_overwrite_basic_test - eviction accounting across the same boundary
 *
 * Return: true on pass.
 */
static bool run_wrap_overwrite_basic_test(void)
{
    struct ringbuf_zcopy *rb =
        ringbuf_zcopy_create(sizeof(struct message), 4, true);
    if (!rb) {
        fprintf(stderr, "wrap_overwrite_basic: ringbuf_zcopy_create failed\n");
        return false;
    }
    ringbuf_zcopy_test_seed_counters(rb, SEED_BASE);
    bool ok = run_overwrite_basic_on(rb, "wrap_overwrite_basic");
    ringbuf_zcopy_destroy(rb);
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
    struct ringbuf_zcopy *rb =
        ringbuf_zcopy_create(sizeof(struct message), 16, false);
    if (!rb) {
        fprintf(stderr,
                "wrap_lossless_concurrent: ringbuf_zcopy_create failed\n");
        return false;
    }
    ringbuf_zcopy_test_seed_counters(rb, SEED_BASE);
    bool ok = run_lossless_transfer(rb, 200000u, "wrap_lossless_concurrent");
    ringbuf_zcopy_destroy(rb);
    return ok;
}

/**
 * run_wrap_overwrite_stress_test - concurrent eviction coverage of the boundary
 *
 * Return: true on pass.
 */
static bool run_wrap_overwrite_stress_test(void)
{
    struct ringbuf_zcopy *rb =
        ringbuf_zcopy_create(sizeof(struct message), 4, true);
    if (!rb) {
        fprintf(stderr, "wrap_overwrite_stress: ringbuf_zcopy_create failed\n");
        return false;
    }
    ringbuf_zcopy_test_seed_counters(rb, SEED_BASE);
    bool ok = run_overwrite_stress_on(rb, 200000u, "wrap_overwrite_stress");
    ringbuf_zcopy_destroy(rb);
    return ok;
}

/**
 * run_wrap_parallel_fill_test - parallel fill across the counter boundary
 *
 * The stamps are what cross SIZE_MAX here alongside the counters, and they
 * are also what the fillers publish through, so this is the case that would
 * catch a stamp comparison written as an absolute magnitude rather than a
 * signed difference.
 *
 * Return: true on pass.
 */
static bool run_wrap_parallel_fill_test(void)
{
    struct ringbuf_zcopy *rb =
        ringbuf_zcopy_create(sizeof(struct message), 4, true);
    if (!rb) {
        fprintf(stderr, "wrap_parallel_fill: ringbuf_zcopy_create failed\n");
        return false;
    }
    ringbuf_zcopy_test_seed_counters(rb, SEED_BASE);
    bool ok = run_parallel_fill_on(rb, 200000u, true, "wrap_parallel_fill");
    ringbuf_zcopy_destroy(rb);
    return ok;
}

#endif /* RINGBUF_TEST_HOOKS */

/**
 * main - run all fourteen cases unconditionally
 *
 * There is no filter flag; the five wrap_* cases are present only under
 * -DRINGBUF_TEST_HOOKS.
 *
 * Return: 0 if every case passed, 1 otherwise.
 */
int main(void)
{
    bool ok = true;

    /* Mirroring test_ringbuf_mutex.c, through push(). */
    ok &= run_lossless_test();
    ok &= run_overwrite_basic_test();
    ok &= run_overwrite_stress_test();

    /* Specific to the split push. */
    ok &= run_reserve_basic_test();
    ok &= run_ooo_submit_test();
    ok &= run_reserve_exhaust_test();
    ok &= run_unsubmitted_blocks_evict_test();
    ok &= run_parallel_fill_test();
    ok &= run_parallel_fill_overwrite_test();

#ifdef RINGBUF_TEST_HOOKS
    ok &= run_wrap_boundary_test();
    ok &= run_wrap_overwrite_basic_test();
    ok &= run_wrap_lossless_concurrent_test();
    ok &= run_wrap_overwrite_stress_test();
    ok &= run_wrap_parallel_fill_test();
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
