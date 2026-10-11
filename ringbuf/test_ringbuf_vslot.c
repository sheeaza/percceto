#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ringbuf_vslot.h"

/* Largest message body any test pushes. Chosen so a message spans several
 * records' worth of a small ring, which is what makes the eviction loop
 * retire more than one record per push. */
#define MAX_SIZE 96u

/* Smallest body, so every message still carries its sequence number. */
#define MIN_SIZE (sizeof(uint64_t))

/* Upper bound on consecutive failed push/pop attempts before a test
 * declares a stall. Legitimate contention resolves within a few thousand
 * yields even when the peer thread loses its timeslice, so this is a large
 * safety margin -- but it is bounded, so a regression that wedges the
 * buffer fails the suite instead of hanging it. */
#define MAX_SPIN 2000000u

/**
 * size_for_seq - body length this test assigns to a sequence number
 * @seq: sequence number
 *
 * Spreads lengths over [MIN_SIZE, MAX_SIZE] with a multiplicative hash, so
 * consecutive messages differ in size and records land at every alignment
 * within the ring. Deterministic, so the consumer can recompute the length
 * it should have received from the seq it did receive -- which is what
 * turns a mis-sized record into a test failure even when its bytes are
 * intact.
 *
 * Return: the body length in bytes.
 */
static size_t size_for_seq(uint64_t seq)
{
    uint64_t span = MAX_SIZE - MIN_SIZE + 1;
    return MIN_SIZE + (size_t)((seq * 2654435761u) % span);
}

/**
 * fill_message_sized - write a message body of an explicit length
 * @buf: at least @size bytes
 * @seq: sequence number to encode
 * @size: body length; must be at least sizeof(uint64_t)
 *
 * Layout is the 8-byte seq followed by a seq-derived byte pattern, so both
 * the identity and the integrity of a message are checkable on receipt.
 */
static void fill_message_sized(unsigned char *buf, uint64_t seq, size_t size)
{
    memcpy(buf, &seq, sizeof(seq));
    for (size_t i = sizeof(seq); i < size; i++)
        buf[i] = (unsigned char)((seq + i) & 0xFF);
}

/**
 * fill_message - write a message body for a sequence number
 * @buf: at least size_for_seq(@seq) bytes
 * @seq: sequence number to encode
 *
 * Return: the body length written.
 */
static size_t fill_message(unsigned char *buf, uint64_t seq)
{
    size_t size = size_for_seq(seq);

    fill_message_sized(buf, seq, size);
    return size;
}

/**
 * message_bytes_ok - check a received body against a seq and length
 * @buf: received body
 * @len: length to check
 * @seq: sequence number the pattern should derive from
 *
 * Return: true if every byte matches what fill_message_sized() writes.
 */
static bool message_bytes_ok(const unsigned char *buf, size_t len, uint64_t seq)
{
    uint64_t got;

    if (len < sizeof(got))
        return false;
    memcpy(&got, buf, sizeof(got));
    if (got != seq)
        return false;

    for (size_t i = sizeof(got); i < len; i++) {
        if (buf[i] != (unsigned char)((seq + i) & 0xFF))
            return false;
    }
    return true;
}

/**
 * message_ok - check a received message against what its seq implies
 * @buf: received body
 * @len: received length, as reported by ringbuf_vslot_pop
 * @seq: out-parameter for the decoded sequence number, set on EVERY return
 *	so a caller's failure message can print it unconditionally. A body
 *	too short to hold one reads back as 0.
 *
 * Checks the length first, since a wrong length is the failure mode
 * specific to this variant, then the payload pattern.
 *
 * Return: true if @len and every byte match what fill_message() writes.
 */
static bool message_ok(const unsigned char *buf, size_t len, uint64_t *seq)
{
    *seq = 0;
    if (len < sizeof(*seq))
        return false;
    memcpy(seq, buf, sizeof(*seq));

    if (len != size_for_seq(*seq))
        return false;
    return message_bytes_ok(buf, len, *seq);
}

/* ---- Lossless transfer (overwrite_on_full = false) ---- */

struct lossless_args {
    struct ringbuf_vslot *rb;
    uint64_t num_messages;
    bool failed;
    const char *label;
};

/**
 * lossless_producer_main - push num_messages variable-size messages
 * @arg: struct lossless_args
 *
 * Return: NULL always; failure is reported via the args' @failed field.
 */
static void *lossless_producer_main(void *arg)
{
    struct lossless_args *a = arg;

    for (uint64_t seq = 0; seq < a->num_messages; seq++) {
        unsigned char body[MAX_SIZE];
        size_t size = fill_message(body, seq);

        uint64_t spins = 0;
        while (!ringbuf_vslot_push(a->rb, body, size)) {
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
 * lossless_consumer_main - pop num_messages, checking order, length, bytes
 * @arg: struct lossless_args
 *
 * Return: NULL always; failure is reported via the args' @failed field.
 */
static void *lossless_consumer_main(void *arg)
{
    struct lossless_args *a = arg;

    for (uint64_t expected_seq = 0; expected_seq < a->num_messages;
         expected_seq++) {
        unsigned char body[MAX_SIZE];
        size_t len;

        uint64_t spins = 0;
        while (!ringbuf_vslot_pop(a->rb, body, sizeof(body), &len)) {
            if (++spins > MAX_SPIN) {
                fprintf(stderr,
                        "%s: consumer stalled waiting for seq %" PRIu64 "\n",
                        a->label, expected_seq);
                a->failed = true;
                return NULL;
            }
            sched_yield();
        }

        uint64_t seq;
        if (!message_ok(body, len, &seq) || seq != expected_seq) {
            fprintf(stderr,
                    "%s: mismatch at expected seq %" PRIu64 ", got seq %" PRIu64
                    " len %zu (expected len %zu)\n",
                    a->label, expected_seq, seq, len,
                    size_for_seq(expected_seq));
            a->failed = true;
            return NULL;
        }
    }
    return NULL;
}

/**
 * run_lossless_transfer - two-thread lossless transfer over a buffer
 * @rb: buffer created with overwrite_on_full = false
 * @n: number of messages to send
 * @label: name used in progress and failure messages
 *
 * Return: true if every message arrived in order, intact, at its own
 * length.
 */
static bool run_lossless_transfer(struct ringbuf_vslot *rb, uint64_t n,
                                  const char *label)
{
    struct lossless_args pargs = {
        .rb = rb, .num_messages = n, .failed = false, .label = label
    };
    struct lossless_args cargs = {
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
    /* A small ring relative to MAX_SIZE forces constant wraparound. */
    struct ringbuf_vslot *rb = ringbuf_vslot_create(MAX_SIZE, 512, false);
    if (!rb) {
        fprintf(stderr, "lossless: ringbuf_vslot_create failed\n");
        return false;
    }
    bool ok = run_lossless_transfer(rb, 2000000u, "lossless");
    ringbuf_vslot_destroy(rb);
    return ok;
}

/* ---- Overwrite mode, single-threaded eviction check ---- */

/**
 * run_overwrite_basic_on - single-threaded eviction check
 * @rb: buffer created with overwrite_on_full = true
 * @label: name used in progress and failure messages
 *
 * Pushes messages with no pops until the ring has certainly cycled, then
 * drains. Unlike the fixed-size variants the expected drop count cannot be
 * a constant -- it depends on the sizes pushed -- so this derives the
 * survivors by replaying the same size sequence: the surviving suffix is
 * the longest one whose records fit in the capacity.
 *
 * Return: true if the drop count and every surviving message match.
 */
static bool run_overwrite_basic_on(struct ringbuf_vslot *rb, const char *label)
{
    const uint64_t pushed = 200;

    for (uint64_t seq = 0; seq < pushed; seq++) {
        unsigned char body[MAX_SIZE];
        size_t size = fill_message(body, seq);
        if (!ringbuf_vslot_push(rb, body, size)) {
            fprintf(stderr, "%s: push unexpectedly failed at seq %" PRIu64 "\n",
                    label, seq);
            return false;
        }
    }

    /* Replay the sizes to find the oldest seq that can still be present:
     * walk back from the newest, accumulating padded record lengths, until
     * the next one would not fit. */
    size_t capacity = ringbuf_vslot_capacity_bytes(rb);
    size_t used = 0;
    uint64_t first_alive = pushed;
    while (first_alive > 0) {
        size_t rec = ((4 + size_for_seq(first_alive - 1)) + 3) & ~(size_t)3;
        if (used + rec > capacity)
            break;
        used += rec;
        first_alive--;
    }

    size_t dropped = ringbuf_vslot_dropped_count(rb);
    if (dropped != first_alive) {
        fprintf(stderr, "%s: expected %" PRIu64 " dropped, got %zu\n", label,
                first_alive, dropped);
        return false;
    }

    for (uint64_t expected_seq = first_alive; expected_seq < pushed;
         expected_seq++) {
        unsigned char body[MAX_SIZE];
        size_t len;

        if (!ringbuf_vslot_pop(rb, body, sizeof(body), &len)) {
            fprintf(stderr, "%s: pop unexpectedly failed at seq %" PRIu64 "\n",
                    label, expected_seq);
            return false;
        }

        uint64_t seq;
        if (!message_ok(body, len, &seq) || seq != expected_seq) {
            fprintf(stderr,
                    "%s: mismatch, expected seq %" PRIu64 ", got %" PRIu64
                    " len %zu\n",
                    label, expected_seq, seq, len);
            return false;
        }
    }

    unsigned char body[MAX_SIZE];
    size_t len;
    if (ringbuf_vslot_pop(rb, body, sizeof(body), &len)) {
        fprintf(stderr, "%s: expected empty buffer after draining\n", label);
        return false;
    }

    printf("%s: PASS (dropped=%zu, survivors=%" PRIu64 ")\n", label, dropped,
           pushed - first_alive);
    return true;
}

/**
 * run_overwrite_basic_test - run the eviction check on a fresh buffer
 *
 * Return: true on pass.
 */
static bool run_overwrite_basic_test(void)
{
    struct ringbuf_vslot *rb = ringbuf_vslot_create(MAX_SIZE, 512, true);
    if (!rb) {
        fprintf(stderr, "overwrite_basic: ringbuf_vslot_create failed\n");
        return false;
    }
    bool ok = run_overwrite_basic_on(rb, "overwrite_basic");
    ringbuf_vslot_destroy(rb);
    return ok;
}

/* ---- Overwrite mode, concurrent stress. A tiny ring maximizes
 * eviction-vs-read contention on the one shared boundary record. ---- */

struct stress_producer_args {
    struct ringbuf_vslot *rb;
    uint64_t num_messages;
    atomic_bool *done;
    bool failed;
    const char *label;
};

struct stress_consumer_args {
    struct ringbuf_vslot *rb;
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
        unsigned char body[MAX_SIZE];
        size_t size = fill_message(body, seq);

        /* In overwrite mode, push only fails on the rare transient lock
         * contention with an in-flight pop on the oldest record. */
        uint64_t spins = 0;
        while (!ringbuf_vslot_push(a->rb, body, size)) {
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
 *	is done, checking lengths, payloads, and monotonic seq
 * @arg: struct stress_consumer_args
 *
 * Gaps in seq are expected -- those are evictions -- but a repeat or a step
 * backwards means a message was handed out twice.
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
        unsigned char body[MAX_SIZE];
        size_t len;

        if (!ringbuf_vslot_pop(a->rb, body, sizeof(body), &len)) {
            if (atomic_load_explicit(a->done, memory_order_acquire)) {
                if (!ringbuf_vslot_pop(a->rb, body, sizeof(body), &len))
                    break; /* producer finished and buffer fully drained */
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

        uint64_t seq;
        if (!message_ok(body, len, &seq)) {
            a->corrupted = true;
            break;
        }
        if (have_prev && seq <= prev_seq) {
            a->out_of_order = true;
            break;
        }

        have_prev = true;
        prev_seq = seq;
        a->received++;
        a->last_seq = seq;
    }
    return NULL;
}

/**
 * run_overwrite_stress_on - concurrent eviction stress over a buffer
 * @rb: buffer created with overwrite_on_full = true
 * @n: number of messages to push
 * @label: name used in progress and failure messages
 *
 * Return: true if lengths and payloads were intact, seqs strictly
 * increasing, the final message observed, and received + dropped == @n.
 */
static bool run_overwrite_stress_on(struct ringbuf_vslot *rb, uint64_t n,
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

    size_t dropped = ringbuf_vslot_dropped_count(rb);

    if (pargs.failed || cargs.stalled)
        return false;
    if (cargs.corrupted) {
        fprintf(stderr, "%s: payload or length corruption detected\n", label);
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
    struct ringbuf_vslot *rb = ringbuf_vslot_create(MAX_SIZE, 512, true);
    if (!rb) {
        fprintf(stderr, "overwrite_stress: ringbuf_vslot_create failed\n");
        return false;
    }
    bool ok = run_overwrite_stress_on(rb, 2000000u, "overwrite_stress");
    ringbuf_vslot_destroy(rb);
    return ok;
}

/* ---- Variant-specific cases: the mirror, and the two truncation paths.
 * None of these have an analogue in the fixed-size variants. ---- */

/**
 * run_straddle_test - drive records across the physical end of the ring
 *
 * The mirrored mapping is what makes a record spanning the end contiguous,
 * so this targets it directly. For each tail gap in [4, 64] it walks
 * write_idx to exactly `capacity - gap` and then pushes a body large
 * enough to run past the end, so the split lands at a different point in
 * the body each time.
 *
 * Positioning is done by pushing and popping ONE record at a time, which
 * advances both counters together and leaves the ring empty -- so the
 * offset is reached without ever filling a lossless buffer.
 *
 * Note the header itself never splits: record starts are 4-byte aligned
 * and the capacity is a multiple of 4, so a 4-byte header always lies
 * entirely before the end. That is exactly what REC_ALIGN buys, and the
 * gap == 4 case pins it down -- the header occupies the final 4 bytes and
 * the whole body is past them, through the mirror.
 *
 * Return: true on pass.
 */
static bool run_straddle_test(void)
{
    struct ringbuf_vslot *rb = ringbuf_vslot_create(MAX_SIZE, 4096, false);
    if (!rb) {
        fprintf(stderr, "straddle: ringbuf_vslot_create failed\n");
        return false;
    }

    size_t capacity = ringbuf_vslot_capacity_bytes(rb);
    bool ok = false;
    size_t tested = 0;
    /* Byte counter tracked in parallel with the buffer's own write_idx.
     * Every push below is matched by a pop, so this stays exact. */
    size_t at = 0;
    uint64_t seq = 0;

    for (size_t gap = 4; gap <= 64; gap += 4) {
        size_t target = capacity - gap;

        /* Walk to `target` one round-trip at a time. The step is a body of
         * MIN_SIZE (a 16-byte record), except when fewer than 16 bytes
         * remain, where a 4-byte record (zero-length body) closes the
         * distance exactly. */
        while (at != target) {
            size_t remaining = target - at;
            size_t body_size = remaining >= 16 ? MIN_SIZE : 0;
            size_t rec = body_size ? 16 : 4;
            unsigned char body[MAX_SIZE];
            size_t len;

            fill_message_sized(body, seq, body_size);
            if (!ringbuf_vslot_push(rb, body, body_size)) {
                fprintf(stderr,
                        "straddle: positioning push failed at offset %zu\n",
                        at);
                goto out;
            }
            if (!ringbuf_vslot_pop(rb, body, sizeof(body), &len)) {
                fprintf(stderr,
                        "straddle: positioning pop failed at offset %zu\n", at);
                goto out;
            }
            if (len != body_size) {
                fprintf(stderr,
                        "straddle: positioning len %zu != %zu at offset %zu\n",
                        len, body_size, at);
                goto out;
            }
            at = (at + rec) & (capacity - 1);
            seq++;
        }

        /* write_idx now sits `gap` bytes from the end, so this record's
         * body necessarily runs past it. */
        {
            size_t body_size = MAX_SIZE;
            unsigned char sent[MAX_SIZE];
            unsigned char got[MAX_SIZE];
            size_t len;

            fill_message_sized(sent, seq, body_size);
            if (!ringbuf_vslot_push(rb, sent, body_size)) {
                fprintf(stderr, "straddle[gap=%zu]: push failed\n", gap);
                goto out;
            }
            if (!ringbuf_vslot_pop(rb, got, sizeof(got), &len)) {
                fprintf(stderr, "straddle[gap=%zu]: pop reported empty\n", gap);
                goto out;
            }
            if (len != body_size) {
                fprintf(stderr, "straddle[gap=%zu]: len %zu != %zu\n", gap, len,
                        body_size);
                goto out;
            }
            if (!message_bytes_ok(got, len, seq)) {
                fprintf(stderr,
                        "straddle[gap=%zu]: corruption in a record spanning "
                        "the ring end\n",
                        gap);
                goto out;
            }
            if (memcmp(got, sent, body_size) != 0) {
                fprintf(stderr, "straddle[gap=%zu]: bytes differ\n", gap);
                goto out;
            }

            at = (at + (((4 + body_size) + 3) & ~(size_t)3)) & (capacity - 1);
            seq++;
            tested++;
        }
    }

    if (!ringbuf_vslot_is_empty_approx(rb)) {
        fprintf(stderr, "straddle: buffer should be empty at the end\n");
        goto out;
    }

    ok = true;
    printf("straddle: PASS (%zu records spanning the ring end)\n", tested);

out:
    ringbuf_vslot_destroy(rb);
    return ok;
}

/**
 * run_truncate_test - producer-side truncation of an oversize message
 *
 * A push longer than max_msg_size must still succeed, must report itself in
 * truncated_count(), and must come back as exactly max_msg_size bytes with its
 * prefix intact.
 *
 * Return: true on pass.
 */
static bool run_truncate_test(void)
{
    struct ringbuf_vslot *rb = ringbuf_vslot_create(MAX_SIZE, 4096, false);
    if (!rb) {
        fprintf(stderr, "truncate: ringbuf_vslot_create failed\n");
        return false;
    }

    bool ok = false;
    const size_t oversize = MAX_SIZE + 37;
    unsigned char *big = malloc(oversize);
    if (!big) {
        fprintf(stderr, "truncate: malloc failed\n");
        goto out;
    }
    for (size_t i = 0; i < oversize; i++)
        big[i] = (unsigned char)(i & 0xFF);

    if (!ringbuf_vslot_push(rb, big, oversize)) {
        fprintf(stderr, "truncate: oversize push failed (should truncate)\n");
        goto out;
    }
    if (ringbuf_vslot_truncated_count(rb) != 1) {
        fprintf(stderr, "truncate: expected truncated_count 1, got %zu\n",
                ringbuf_vslot_truncated_count(rb));
        goto out;
    }

    {
        unsigned char body[MAX_SIZE * 2];
        size_t len;

        if (!ringbuf_vslot_pop(rb, body, sizeof(body), &len)) {
            fprintf(stderr, "truncate: pop failed\n");
            goto out;
        }
        if (len != MAX_SIZE) {
            fprintf(stderr, "truncate: expected len %u, got %zu\n", MAX_SIZE,
                    len);
            goto out;
        }
        if (memcmp(body, big, MAX_SIZE) != 0) {
            fprintf(stderr, "truncate: surviving prefix does not match\n");
            goto out;
        }
    }

    /* A zero-length message is legal and must round-trip as one. */
    if (!ringbuf_vslot_push(rb, NULL, 0)) {
        fprintf(stderr, "truncate: zero-length push failed\n");
        goto out;
    }
    {
        unsigned char body[MAX_SIZE];
        size_t len;

        if (!ringbuf_vslot_pop(rb, body, sizeof(body), &len)) {
            fprintf(stderr, "truncate: zero-length pop failed\n");
            goto out;
        }
        if (len != 0) {
            fprintf(stderr, "truncate: expected zero-length, got %zu\n", len);
            goto out;
        }
    }

    ok = true;
    printf("truncate: PASS (oversize clamped to %u, zero-length round-trip)\n",
           MAX_SIZE);

out:
    free(big);
    ringbuf_vslot_destroy(rb);
    return ok;
}

/**
 * run_pop_cap_test - consumer-side truncation reporting
 *
 * pop() reports the STORED length rather than the number of bytes copied,
 * so a caller can detect that its buffer was too small by testing
 * *len > cap. This checks that, and that pop writes exactly cap bytes and
 * not one more.
 *
 * Return: true on pass.
 */
static bool run_pop_cap_test(void)
{
    struct ringbuf_vslot *rb = ringbuf_vslot_create(MAX_SIZE, 4096, false);
    if (!rb) {
        fprintf(stderr, "pop_cap: ringbuf_vslot_create failed\n");
        return false;
    }

    bool ok = false;
    const uint64_t seq = 7;
    unsigned char sent[MAX_SIZE];
    size_t size = fill_message(sent, seq);
    const size_t cap = 16;

    if (size <= cap) {
        fprintf(stderr, "pop_cap: test needs size > cap (%zu <= %zu)\n", size,
                cap);
        goto out;
    }

    if (!ringbuf_vslot_push(rb, sent, size)) {
        fprintf(stderr, "pop_cap: push failed\n");
        goto out;
    }

    {
        /* Guard byte after the capacity, to catch an overcopy. */
        unsigned char body[cap + 1];
        memset(body, 0xAA, sizeof(body));

        size_t len;
        if (!ringbuf_vslot_pop(rb, body, cap, &len)) {
            fprintf(stderr, "pop_cap: pop failed\n");
            goto out;
        }
        if (len != size) {
            fprintf(stderr,
                    "pop_cap: expected stored len %zu, got %zu (must report "
                    "the stored length, not the copied one)\n",
                    size, len);
            goto out;
        }
        if (memcmp(body, sent, cap) != 0) {
            fprintf(stderr, "pop_cap: copied prefix does not match\n");
            goto out;
        }
        if (body[cap] != 0xAA) {
            fprintf(stderr, "pop_cap: pop wrote past cap\n");
            goto out;
        }
    }

    /* The truncated message is consumed whole, not left partly in the
     * ring: the buffer must now be empty. */
    {
        unsigned char body[MAX_SIZE];
        size_t len;
        if (ringbuf_vslot_pop(rb, body, sizeof(body), &len)) {
            fprintf(stderr, "pop_cap: expected empty after truncated pop\n");
            goto out;
        }
    }

    ok = true;
    printf("pop_cap: PASS (stored len %zu reported through cap %zu)\n", size,
           cap);

out:
    ringbuf_vslot_destroy(rb);
    return ok;
}

#ifdef RINGBUF_TEST_HOOKS

/* ---- Counter-overflow tests.
 *
 * The internal counters are byte counts, monotonic, and only mapped onto
 * the ring via a mask, so they eventually overflow SIZE_MAX. Reaching that
 * organically would take SIZE_MAX bytes of pushes, so these park the
 * counters just below the boundary with ringbuf_vslot_test_seed_counters()
 * and push across it.
 *
 * SEED_BASE is SIZE_MAX - 3 rather than ringbuf.c's SIZE_MAX - 2 because
 * records are 4-byte aligned and the seed must be too. It leaves exactly 4
 * bytes before the overflow, so the first record's header sits at the very
 * end of the ring and its body begins at offset 0 THROUGH THE MIRROR --
 * these cases exercise the mirror and the counter overflow at once.
 * ---- */

#define SEED_BASE (SIZE_MAX - 3)

/**
 * run_wrap_boundary_test - fill, refuse, and drain across the SIZE_MAX
 *	counter boundary
 *
 * Sharp regression test: single-threaded, so a failure can only come from
 * the free-space arithmetic rather than from a race.
 *
 * Return: true on pass.
 */
static bool run_wrap_boundary_test(void)
{
    struct ringbuf_vslot *rb = ringbuf_vslot_create(MAX_SIZE, 4096, false);
    if (!rb) {
        fprintf(stderr, "wrap_boundary: ringbuf_vslot_create failed\n");
        return false;
    }
    ringbuf_vslot_test_seed_counters(rb, SEED_BASE);

    size_t capacity = ringbuf_vslot_capacity_bytes(rb);

    if (!ringbuf_vslot_is_empty_approx(rb)) {
        fprintf(stderr,
                "wrap_boundary: should read empty right after seeding\n");
        goto fail;
    }
    if (ringbuf_vslot_free_bytes_approx(rb) != capacity) {
        fprintf(stderr, "wrap_boundary: expected %zu free, got %zu\n", capacity,
                ringbuf_vslot_free_bytes_approx(rb));
        goto fail;
    }

    /* Fill until the next message will not fit; write_idx crosses SIZE_MAX
     * on the first record. */
    uint64_t pushed = 0;
    for (;;) {
        unsigned char body[MAX_SIZE];
        size_t size = fill_message(body, pushed);
        size_t rec = ((4 + size) + 3) & ~(size_t)3;

        if (ringbuf_vslot_free_bytes_approx(rb) < rec)
            break;
        if (!ringbuf_vslot_push(rb, body, size)) {
            fprintf(stderr,
                    "wrap_boundary: push failed at seq %" PRIu64
                    " with %zu free and rec %zu\n",
                    pushed, ringbuf_vslot_free_bytes_approx(rb), rec);
            goto fail;
        }
        pushed++;
    }

    /* Lossless buffer: a message that does not fit must be refused, not
     * wrap around and clobber unread data. */
    {
        unsigned char body[MAX_SIZE];
        size_t size = fill_message(body, pushed);
        if (ringbuf_vslot_push(rb, body, size)) {
            fprintf(stderr,
                    "wrap_boundary: push succeeded with only %zu free\n",
                    ringbuf_vslot_free_bytes_approx(rb));
            goto fail;
        }
    }

    /* write_idx has now wrapped past SIZE_MAX while read_idx has not. */
    for (uint64_t expected = 0; expected < pushed; expected++) {
        unsigned char body[MAX_SIZE];
        size_t len;

        if (!ringbuf_vslot_pop(rb, body, sizeof(body), &len)) {
            fprintf(stderr,
                    "wrap_boundary: pop reported empty across the SIZE_MAX "
                    "boundary at seq %" PRIu64 "\n",
                    expected);
            goto fail;
        }

        uint64_t seq;
        if (!message_ok(body, len, &seq) || seq != expected) {
            fprintf(stderr,
                    "wrap_boundary: mismatch, expected seq %" PRIu64
                    ", got %" PRIu64 " len %zu\n",
                    expected, seq, len);
            goto fail;
        }
    }

    {
        unsigned char body[MAX_SIZE];
        size_t len;
        if (ringbuf_vslot_pop(rb, body, sizeof(body), &len)) {
            fprintf(stderr,
                    "wrap_boundary: expected empty buffer after draining\n");
            goto fail;
        }
    }
    if (!ringbuf_vslot_is_empty_approx(rb)) {
        fprintf(stderr, "wrap_boundary: should read empty after draining\n");
        goto fail;
    }

    ringbuf_vslot_destroy(rb);
    printf("wrap_boundary: PASS (crossed SIZE_MAX, %" PRIu64 " messages)\n",
           pushed);
    return true;

fail:
    ringbuf_vslot_destroy(rb);
    return false;
}

/**
 * run_wrap_overwrite_basic_test - eviction accounting across the boundary
 *
 * Return: true on pass.
 */
static bool run_wrap_overwrite_basic_test(void)
{
    struct ringbuf_vslot *rb = ringbuf_vslot_create(MAX_SIZE, 512, true);
    if (!rb) {
        fprintf(stderr, "wrap_overwrite_basic: ringbuf_vslot_create failed\n");
        return false;
    }
    ringbuf_vslot_test_seed_counters(rb, SEED_BASE);
    bool ok = run_overwrite_basic_on(rb, "wrap_overwrite_basic");
    ringbuf_vslot_destroy(rb);
    return ok;
}

/**
 * run_wrap_lossless_concurrent_test - concurrent lossless coverage of the
 *	boundary, for ThreadSanitizer
 *
 * Smaller counts than the main stress tests: the boundary is crossed in
 * the first handful of pushes, so the rest is just ordinary running.
 *
 * Return: true on pass.
 */
static bool run_wrap_lossless_concurrent_test(void)
{
    struct ringbuf_vslot *rb = ringbuf_vslot_create(MAX_SIZE, 512, false);
    if (!rb) {
        fprintf(stderr,
                "wrap_lossless_concurrent: ringbuf_vslot_create failed\n");
        return false;
    }
    ringbuf_vslot_test_seed_counters(rb, SEED_BASE);
    bool ok = run_lossless_transfer(rb, 200000u, "wrap_lossless_concurrent");
    ringbuf_vslot_destroy(rb);
    return ok;
}

/**
 * run_wrap_overwrite_stress_test - concurrent eviction coverage of the
 *	boundary
 *
 * Return: true on pass.
 */
static bool run_wrap_overwrite_stress_test(void)
{
    struct ringbuf_vslot *rb = ringbuf_vslot_create(MAX_SIZE, 512, true);
    if (!rb) {
        fprintf(stderr, "wrap_overwrite_stress: ringbuf_vslot_create failed\n");
        return false;
    }
    ringbuf_vslot_test_seed_counters(rb, SEED_BASE);
    bool ok = run_overwrite_stress_on(rb, 200000u, "wrap_overwrite_stress");
    ringbuf_vslot_destroy(rb);
    return ok;
}

#endif /* RINGBUF_TEST_HOOKS */

/**
 * main - run all ten cases unconditionally
 *
 * There is no filter flag; the four wrap_* cases are present only under
 * -DRINGBUF_TEST_HOOKS.
 *
 * Return: 0 if every case passed, 1 otherwise.
 */
int main(void)
{
    bool ok = true;
    ok &= run_lossless_test();
    ok &= run_overwrite_basic_test();
    ok &= run_overwrite_stress_test();
    ok &= run_straddle_test();
    ok &= run_truncate_test();
    ok &= run_pop_cap_test();
#ifdef RINGBUF_TEST_HOOKS
    ok &= run_wrap_boundary_test();
    ok &= run_wrap_overwrite_basic_test();
    ok &= run_wrap_lossless_concurrent_test();
    ok &= run_wrap_overwrite_stress_test();
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
