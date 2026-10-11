/*
 * Multi-producer tests for the reserve/submit variant.
 *
 * These exercise a claim that ringbuf_zcopy.h makes but test_ringbuf_zcopy.c
 * cannot check: SEVERAL threads may reserve, provided every reserve is
 * serialized by a mutex the CALLER supplies. Nothing in ringbuf_zcopy.c
 * synchronizes producers against each other, so if that claim is wrong, it is
 * wrong here and nowhere else.
 *
 * Why this is a separate binary rather than more cases in
 * test_ringbuf_zcopy.c: every reserve and push here must go through
 * gated_reserve()/gated_push(), which take push_mu. Mixing gated and ungated
 * callers in one file would obscure which discipline any given case was
 * demonstrating -- and the discipline is the thing under test. Keeping them
 * apart means test_ringbuf_zcopy.c stays the single-producer contract,
 * verbatim, and this file is the multi-producer one.
 *
 * The library is shared and unmodified. There is deliberately no
 * ringbuf_zcopy_mpsc.c: the single-producer reasoning in ringbuf_zcopy.c needs
 * reserves SERIALIZED, not same-threaded, so a caller-held mutex satisfies it
 * as written. A separate implementation was built first and came out
 * token-for-token identical, which is what settled it. See CLAUDE.md, "Several
 * producers under a caller-held mutex".
 *
 * Five cases. The one with no single-producer counterpart at all is
 * peer_unblocks_reserve: under SPSC a reserve refused for an unsubmitted slot
 * can only be blocked by the caller's OWN reservation, so retrying is futile;
 * with several producers the blocker is usually a peer, so retrying is the
 * correct response. Same return value, opposite advice.
 *
 * Two harness rules, both consequences of head-of-line blocking:
 *
 *   - A false return from ringbuf_zcopy_pop() does NOT mean empty, because a
 *     reserved-but-unsubmitted slot blocks delivery head-of-line. Consumers
 *     must retry, and may only conclude "drained" once every producer has
 *     finished and submitted.
 *   - A false return from reserve() is routine and must be retried with
 *     push_mu RELEASED, which gated_reserve() ensures by construction.
 */

#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "ringbuf_zcopy.h"

#define PAYLOAD_LEN 24

/* Messages carry their origin, because with several producers a single global
 * sequence order no longer exists: which producer wins the push mutex next is
 * genuinely nondeterministic. Each producer numbers its own stream and the
 * consumer checks the streams independently. */
struct message {
    uint64_t seq;
    uint32_t producer_id;
    unsigned char payload[PAYLOAD_LEN];
};

/* The one mutex the contract requires around reserve()/push(), and the only
 * thing in this file that makes several producers legal. File-scope static, so
 * PTHREAD_MUTEX_INITIALIZER is unambiguously correct and every case shares
 * one: no two cases run concurrently, and within a case all producers must
 * contend on the same lock for the serialization to mean anything. */
static pthread_mutex_t push_mu = PTHREAD_MUTEX_INITIALIZER;

/* Upper bound on consecutive failed reserve/pop attempts before a test
 * declares a stall. Legitimate contention resolves within a few thousand
 * yields even when a peer thread loses its timeslice, so this is a large
 * safety margin -- but it is bounded, so a regression that wedges the buffer
 * (e.g. a slot stuck reading as unsubmitted forever) fails the suite instead
 * of hanging it. */
#define MAX_SPIN 2000000u

/* Four threads contending for one push mutex is enough to make the hand-off
 * between producers -- the edge this arrangement leans on instead of having
 * its own -- genuinely exercised. */
#define NUM_PRODUCERS 4

/**
 * payload_key - the value a message's payload pattern is derived from
 * @seq: the producer's own sequence number
 * @producer_id: which producer sent it
 *
 * Mixes both fields rather than using @seq alone, which test_ringbuf_zcopy.c
 * can afford to do. With several streams the same @seq occurs once per
 * producer, so a seq-only pattern would validate cleanly even if two messages'
 * bytes were crossed. Folding in @producer_id makes that detectable.
 *
 * Return: the key for this (@producer_id, @seq) pair.
 */
static uint64_t payload_key(uint64_t seq, uint32_t producer_id)
{
    return seq * 31u + producer_id;
}

/**
 * fill_payload - write the key-derived payload pattern
 * @payload: PAYLOAD_LEN bytes to fill
 * @key: value from payload_key() the pattern is derived from
 */
static void fill_payload(unsigned char *payload, uint64_t key)
{
    for (int i = 0; i < PAYLOAD_LEN; i++)
        payload[i] = (unsigned char)((key + (uint64_t)i) & 0xFF);
}

/**
 * payload_ok - check a payload against the pattern for a message
 * @payload: PAYLOAD_LEN bytes as received
 * @seq: sequence number the payload should match
 * @producer_id: producer the payload should have come from
 *
 * Return: true if @payload is exactly what fill_payload() writes for this
 * (@producer_id, @seq) pair.
 */
static bool payload_ok(const unsigned char *payload, uint64_t seq,
                       uint32_t producer_id)
{
    unsigned char expected[PAYLOAD_LEN];
    fill_payload(expected, payload_key(seq, producer_id));
    return memcmp(payload, expected, PAYLOAD_LEN) == 0;
}

/**
 * build_message - assemble a message in a caller-supplied buffer
 * @msg: message to fill in
 * @seq: sequence number to stamp it with
 * @producer_id: producer to attribute it to
 *
 * The staging-buffer path, for the cases that go through push().
 */
static void build_message(struct message *msg, uint64_t seq,
                          uint32_t producer_id)
{
    msg->seq = seq;
    msg->producer_id = producer_id;
    fill_payload(msg->payload, payload_key(seq, producer_id));
}

/**
 * fill_in_place - write a whole message directly into a reserved slot
 * @slot: a reservation's data pointer, slot_size >= sizeof(struct message)
 * @seq: sequence number to stamp the message with
 * @producer_id: producer to attribute it to
 *
 * The zero-copy write the variant exists for: the message is assembled in the
 * ring itself, with no staging buffer and no second copy. Called with push_mu
 * RELEASED, which is what lets several producers' fills overlap.
 */
static void fill_in_place(void *slot, uint64_t seq, uint32_t producer_id)
{
    struct message *m = slot;
    m->seq = seq;
    m->producer_id = producer_id;
    fill_payload(m->payload, payload_key(seq, producer_id));
}

/**
 * gated_reserve - reserve a slot under the caller's push mutex
 * @rb: the buffer
 * @rsv: handle to fill in, valid only on a true return
 *
 * The contract's prescribed usage, in the three lines it takes. Holds push_mu
 * across the claim and nothing else: the fill happens after this returns, with
 * the mutex released, which is the whole reason for doing it this way rather
 * than wrapping push(). Releasing before returning also means a caller's retry
 * loop automatically retries with the mutex dropped, letting the peer whose
 * unsubmitted slot is blocking us make progress.
 *
 * Return: whatever ringbuf_zcopy_reserve() returned.
 */
static bool gated_reserve(struct ringbuf_zcopy *rb, struct ringbuf_rsv *rsv)
{
    pthread_mutex_lock(&push_mu);
    bool ok = ringbuf_zcopy_reserve(rb, rsv);
    pthread_mutex_unlock(&push_mu);
    return ok;
}

/**
 * gated_push - push one message under the caller's push mutex
 * @rb: the buffer
 * @data: slot_size bytes to copy in
 *
 * Note what this necessarily does that gated_reserve() does not: it holds
 * push_mu across the payload copy, since the copy happens inside push(). That
 * is the cost of the convenience wrapper, and the reason the parallel-fill
 * cases below use reserve/submit instead.
 *
 * Return: whatever ringbuf_zcopy_push() returned.
 */
static bool gated_push(struct ringbuf_zcopy *rb, const void *data)
{
    pthread_mutex_lock(&push_mu);
    bool ok = ringbuf_zcopy_push(rb, data);
    pthread_mutex_unlock(&push_mu);
    return ok;
}

/* ---- Several producers, one push mutex ----
 *
 * NUM_PRODUCERS threads each run the contract's prescribed loop -- reserve
 * under push_mu, release it, fill, submit -- so fills and submits from
 * different producers genuinely overlap. That is the configuration this file
 * exists for, and the one that exercises the hand-off ringbuf_zcopy.c leans on
 * instead of providing: one producer's relaxed write_idx store reaching the
 * next producer's relaxed load through the mutex's own release/acquire edge.
 *
 * The consumer cannot check a single global order -- which producer wins the
 * mutex next is nondeterministic -- so it checks, per producer, that seqs
 * arrive strictly increasing. That is the property reservation ordering must
 * provide, and the one a broken hand-off destroys: two producers handed the
 * same ticket show up as a duplicate or backwards seq within a stream, or as a
 * payload that does not match its own (producer_id, seq). */

struct mp_producer_args {
    struct ringbuf_zcopy *rb;
    uint32_t producer_id;
    uint64_t num_messages;
    atomic_int *active; /* producers still running; consumer watches it */
    uint64_t submitted;
    bool failed;
    const char *label;
};

struct mp_consumer_args {
    struct ringbuf_zcopy *rb;
    atomic_int *active;
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
 * mp_reserve_producer_main - reserve, fill off-lock, and submit one stream
 * @arg: struct mp_producer_args
 *
 * The canonical multi-producer usage, one thread of several: push_mu is held
 * only across the claim (inside gated_reserve), and the fill and submit happen
 * with it released, so every producer's fill runs concurrently with every
 * other's.
 *
 * Decrements *active on the way out, so the consumer can tell when every
 * producer has finished rather than just one.
 *
 * Return: NULL always; failure is reported via the args' @failed field.
 */
static void *mp_reserve_producer_main(void *arg)
{
    struct mp_producer_args *a = arg;

    for (uint64_t seq = 0; seq < a->num_messages; seq++) {
        struct ringbuf_rsv rsv;

        /* Retry in both modes, with the mutex released between attempts --
         * which gated_reserve() guarantees. That matters more here than in the
         * single-producer file: the slot we are refused is usually blocked by
         * ANOTHER producer's in-flight fill, so our retry only succeeds if
         * that producer can run. Holding push_mu across the retry, or
         * requiring it on the submit path, would deadlock exactly here. */
        uint64_t spins = 0;
        while (!gated_reserve(a->rb, &rsv)) {
            if (++spins > MAX_SPIN) {
                fprintf(stderr,
                        "%s: producer %" PRIu32 " stalled reserving for seq "
                        "%" PRIu64 "\n",
                        a->label, a->producer_id, seq);
                a->failed = true;
                atomic_fetch_sub_explicit(a->active, 1, memory_order_release);
                return NULL;
            }
            sched_yield();
        }

        /* Outside the lock: this is the work being parallelized. */
        fill_in_place(rsv.data, seq, a->producer_id);
        ringbuf_zcopy_submit(a->rb, &rsv);
        a->submitted++;
    }

    atomic_fetch_sub_explicit(a->active, 1, memory_order_release);
    return NULL;
}

/**
 * mp_push_producer_main - push one numbered stream, no reservations
 * @arg: struct mp_producer_args
 *
 * The degenerate configuration: push() copies inside the mutex, so these
 * producers get no fill parallelism at all. Worth testing anyway, because it
 * is the one multi-producer shape where NO reservation is ever outstanding --
 * so reserve's unsubmitted-slot refusal cannot arise and the buffer must
 * behave exactly like ringbuf_mutex under a push mutex.
 *
 * No retry loop, deliberately: overwrite mode's never-fails promise applies to
 * each of these producers exactly as it does to the single producer in
 * test_ringbuf_zcopy.c, so a false return is a hard failure.
 *
 * Return: NULL always; failure is reported via the args' @failed field.
 */
static void *mp_push_producer_main(void *arg)
{
    struct mp_producer_args *a = arg;

    for (uint64_t seq = 0; seq < a->num_messages; seq++) {
        struct message msg;
        build_message(&msg, seq, a->producer_id);

        if (!gated_push(a->rb, &msg)) {
            fprintf(stderr,
                    "%s: producer %" PRIu32 " got false from push in overwrite "
                    "mode at seq %" PRIu64 " (must never happen)\n",
                    a->label, a->producer_id, seq);
            a->failed = true;
            atomic_fetch_sub_explicit(a->active, 1, memory_order_release);
            return NULL;
        }
        a->submitted++;
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
        if (!ringbuf_zcopy_pop(a->rb, &msg)) {
            /* A false return does not mean empty here -- any producer may be
             * mid-fill on the oldest reservation. Only stop once every
             * producer is gone AND a final attempt still yields nothing. Each
             * producer submits before decrementing `active`, so at that point
             * nothing is outstanding. */
            if (atomic_load_explicit(a->active, memory_order_acquire) == 0) {
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

        if (msg.producer_id >= NUM_PRODUCERS) {
            a->bad_producer_id = true;
            break;
        }
        /* Checks the payload against this message's OWN (producer_id, seq), so
         * a fill that was torn or crossed with another producer's fails here
         * even though both halves are individually well-formed. */
        if (!payload_ok(msg.payload, msg.seq, msg.producer_id)) {
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
 * run_mp_on - NUM_PRODUCERS producers against an existing buffer
 * @rb: the buffer, in either mode
 * @producer_main: thread entry point, reserve/submit or push flavored
 * @n: messages per producer
 * @lossless: true if @rb was created with overwrite_on_full = false, in which
 *	case nothing may be dropped and every stream must arrive complete
 * @label: name used in progress and failure messages
 *
 * Return: true if every stream was strictly increasing with intact payloads
 * and the totals reconcile.
 */
static bool run_mp_on(struct ringbuf_zcopy *rb, void *(*producer_main)(void *),
                      uint64_t n, bool lossless, const char *label)
{
    atomic_int active = NUM_PRODUCERS;
    struct mp_producer_args pargs[NUM_PRODUCERS];
    struct mp_consumer_args cargs = { .rb = rb,
                                      .active = &active,
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
                                              .submitted = 0,
                                              .failed = false,
                                              .label = label };
        pthread_create(&producers[i], NULL, producer_main, &pargs[i]);
    }
    for (uint32_t i = 0; i < NUM_PRODUCERS; i++)
        pthread_join(producers[i], NULL);
    pthread_join(consumer, NULL);

    size_t dropped = ringbuf_zcopy_dropped_count(rb);
    uint64_t pushed = n * NUM_PRODUCERS;
    uint64_t submitted = 0;

    for (uint32_t i = 0; i < NUM_PRODUCERS; i++) {
        if (pargs[i].failed)
            return false;
        submitted += pargs[i].submitted;
    }
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
    /* Every reservation was submitted exactly once, so no handle leaked -- an
     * abandoned one would have wedged the consumer instead, but checking the
     * count distinguishes the two failures. */
    if (submitted != pushed) {
        fprintf(stderr, "%s: producers submitted %" PRIu64 " of %" PRIu64 "\n",
                label, submitted, pushed);
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
        /* Nothing may be lost, so each stream must have arrived whole -- and
         * since seqs are strictly increasing per producer and start at 0, a
         * full count means exactly 0..n-1 with no gaps. */
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
 * run_mp_push_test - NUM_PRODUCERS producers through push() alone
 *
 * No reservation is ever outstanding, so this is the shape where the buffer
 * must behave exactly like ringbuf_mutex under a push mutex -- including
 * overwrite mode's never-fails promise. A false return here would mean that
 * equivalence has broken for several producers even though the single-producer
 * cases in test_ringbuf_zcopy.c still pass.
 *
 * Return: true on pass.
 */
static bool run_mp_push_test(void)
{
    struct ringbuf_zcopy *rb =
        ringbuf_zcopy_create(sizeof(struct message), 4, true);
    if (!rb) {
        fprintf(stderr, "mpsc_push: ringbuf_zcopy_create failed\n");
        return false;
    }
    bool ok = run_mp_on(rb, mp_push_producer_main, 200000u, false, "mpsc_push");
    ringbuf_zcopy_destroy(rb);
    return ok;
}

/**
 * run_mp_parallel_fill_test - NUM_PRODUCERS producers, lossless, nothing lost
 *
 * The headline case: several threads reserving under one mutex, filling in
 * parallel outside it, and every stream arriving complete and in order.
 *
 * Return: true on pass.
 */
static bool run_mp_parallel_fill_test(void)
{
    struct ringbuf_zcopy *rb =
        ringbuf_zcopy_create(sizeof(struct message), 16, false);
    if (!rb) {
        fprintf(stderr, "mpsc_parallel_fill: ringbuf_zcopy_create failed\n");
        return false;
    }
    bool ok = run_mp_on(rb, mp_reserve_producer_main, 300000u, true,
                        "mpsc_parallel_fill");
    ringbuf_zcopy_destroy(rb);
    return ok;
}

/**
 * run_mp_parallel_fill_overwrite_test - NUM_PRODUCERS producers against a tiny
 *	overwrite-mode ring
 *
 * Capacity 4 with four producers is the maximum-contention case: every producer
 * is usually holding one of the four slots mid-fill, so reserves are refused
 * constantly and almost every successful one evicts. It is where an eviction
 * that failed to respect an unsubmitted slot would corrupt a payload, and where
 * a missing re-stamp in the eviction path shows up as a duplicate seq a lap
 * later.
 *
 * Return: true on pass.
 */
static bool run_mp_parallel_fill_overwrite_test(void)
{
    struct ringbuf_zcopy *rb =
        ringbuf_zcopy_create(sizeof(struct message), 4, true);
    if (!rb) {
        fprintf(stderr,
                "mpsc_parallel_fill_overwrite: ringbuf_zcopy_create failed\n");
        return false;
    }
    bool ok = run_mp_on(rb, mp_reserve_producer_main, 300000u, false,
                        "mpsc_parallel_fill_overwrite");
    ringbuf_zcopy_destroy(rb);
    return ok;
}

/* ---- One producer's submit unblocks another's reserve ---- */

#define PEER_HELD_READY 1
#define PEER_MAIN_REFUSED 2
#define PEER_SUBMITTED 3

struct peer_args {
    struct ringbuf_zcopy *rb;
    atomic_int *phase;
    bool failed;
};

/**
 * await_phase - spin until the handshake reaches @want
 * @phase: the shared phase variable
 * @want: value to wait for
 *
 * Acquire load, so whatever the peer did before advancing the phase is visible
 * once this returns.
 *
 * Return: true if @want was reached; false after MAX_SPIN yields, which means
 * the peer is stuck.
 */
static bool await_phase(atomic_int *phase, int want)
{
    for (uint64_t spins = 0; spins <= MAX_SPIN; spins++) {
        if (atomic_load_explicit(phase, memory_order_acquire) == want)
            return true;
        sched_yield();
    }
    return false;
}

/**
 * peer_holder_main - hold a reservation unsubmitted, then release it
 * @arg: struct peer_args
 *
 * Producer A in run_peer_unblocks_reserve_test. Reserves one slot, fills it,
 * and waits -- so the slot is legitimately mid-fill from another thread's point
 * of view -- then submits once the main thread has confirmed it was refused.
 *
 * Return: NULL always; failure is reported via the args' @failed field.
 */
static void *peer_holder_main(void *arg)
{
    struct peer_args *a = arg;
    struct ringbuf_rsv held;

    if (!gated_reserve(a->rb, &held)) {
        fprintf(stderr, "peer_unblocks_reserve: holder's reserve failed on an "
                        "empty ring\n");
        a->failed = true;
        atomic_store_explicit(a->phase, PEER_SUBMITTED, memory_order_release);
        return NULL;
    }
    fill_in_place(held.data, 0, 0);

    atomic_store_explicit(a->phase, PEER_HELD_READY, memory_order_release);

    if (!await_phase(a->phase, PEER_MAIN_REFUSED)) {
        fprintf(stderr, "peer_unblocks_reserve: holder timed out waiting for "
                        "the refusal\n");
        a->failed = true;
    }

    /* The submit that clears the blockage. Note it happens WITHOUT push_mu --
     * the main thread may be inside gated_reserve right now, and if submit
     * needed that mutex the two would deadlock. That is why submit must never
     * require the caller's lock. */
    ringbuf_zcopy_submit(a->rb, &held);
    atomic_store_explicit(a->phase, PEER_SUBMITTED, memory_order_release);
    return NULL;
}

/**
 * run_peer_unblocks_reserve_test - a peer's unsubmitted slot blocks us, and
 *	only its submit clears it
 *
 * The case with no single-producer counterpart. In test_ringbuf_zcopy.c a
 * reserve refused for an unsubmitted slot could only ever be blocked by the
 * single producer's OWN reservation, so retrying without submitting spun
 * forever. Here the blocking reservation belongs to another thread, so the
 * refusal is transient and retrying is the correct response -- the practical
 * difference between the two contracts, on identical code.
 *
 * Deterministic rather than stress-shaped: a three-step handshake pins the
 * exact moment of each check, so a failure names the broken transition instead
 * of showing up as corruption under load.
 *
 * Return: true on pass.
 */
static bool run_peer_unblocks_reserve_test(void)
{
    struct ringbuf_zcopy *rb =
        ringbuf_zcopy_create(sizeof(struct message), 4, true);
    if (!rb) {
        fprintf(stderr, "peer_unblocks_reserve: ringbuf_zcopy_create failed\n");
        return false;
    }

    bool ok = false;
    atomic_int phase = 0;
    struct peer_args hargs = { .rb = rb, .phase = &phase, .failed = false };
    struct ringbuf_rsv mine;
    struct message msg;
    size_t dropped;
    pthread_t holder;

    pthread_create(&holder, NULL, peer_holder_main, &hargs);

    if (!await_phase(&phase, PEER_HELD_READY)) {
        fprintf(stderr, "peer_unblocks_reserve: holder never reserved\n");
        goto join;
    }

    /* This thread, as a second producer, fills the remaining three slots. The
     * ring is now full, and its oldest slot is a reservation belonging to a
     * DIFFERENT thread. */
    for (uint64_t seq = 1; seq < 4; seq++) {
        struct message tmp;
        build_message(&tmp, seq, 0);
        if (!gated_push(rb, &tmp)) {
            fprintf(stderr,
                    "peer_unblocks_reserve: push failed at seq %" PRIu64
                    " with room\n",
                    seq);
            goto join;
        }
    }

    /* Overwrite mode, full ring -- and still a refusal, because the victim
     * would be a slot another producer is filling. */
    if (gated_reserve(rb, &mine)) {
        fprintf(stderr, "peer_unblocks_reserve: evicted a peer's unsubmitted "
                        "slot\n");
        goto join;
    }
    if (ringbuf_zcopy_dropped_count(rb) != 0) {
        fprintf(stderr,
                "peer_unblocks_reserve: counted a drop for a refused reserve "
                "(dropped=%zu)\n",
                ringbuf_zcopy_dropped_count(rb));
        goto join;
    }

    atomic_store_explicit(&phase, PEER_MAIN_REFUSED, memory_order_release);
    if (!await_phase(&phase, PEER_SUBMITTED)) {
        fprintf(stderr, "peer_unblocks_reserve: holder never submitted\n");
        goto join;
    }

    /* The peer's submit turned an un-evictable slot into an evictable one.
     * This is the retry that a single-producer caller could never usefully
     * make. */
    if (!gated_reserve(rb, &mine)) {
        fprintf(stderr, "peer_unblocks_reserve: reserve still refused after "
                        "the peer submitted\n");
        goto join;
    }
    fill_in_place(mine.data, 4, 0);
    ringbuf_zcopy_submit(rb, &mine);

    dropped = ringbuf_zcopy_dropped_count(rb);
    if (dropped != 1) {
        fprintf(stderr, "peer_unblocks_reserve: expected 1 dropped, got %zu\n",
                dropped);
        goto join;
    }

    /* The peer's seq 0 was the eviction victim, so 1..4 survive in reservation
     * order regardless of which thread produced them. */
    for (uint64_t expected_seq = 1; expected_seq <= 4; expected_seq++) {
        if (!ringbuf_zcopy_pop(rb, &msg) || msg.seq != expected_seq ||
            !payload_ok(msg.payload, expected_seq, 0)) {
            fprintf(stderr, "peer_unblocks_reserve: expected seq %" PRIu64 "\n",
                    expected_seq);
            goto join;
        }
    }
    if (ringbuf_zcopy_pop(rb, &msg)) {
        fprintf(stderr,
                "peer_unblocks_reserve: expected empty after draining\n");
        goto join;
    }

    ok = true;

join:
    /* Unblock the holder on every error path, so a failure cannot hang the
     * suite in pthread_join. */
    atomic_store_explicit(&phase, PEER_MAIN_REFUSED, memory_order_release);
    pthread_join(holder, NULL);
    if (hargs.failed)
        ok = false;
    if (ok)
        printf("peer_unblocks_reserve: PASS (dropped=1)\n");
    ringbuf_zcopy_destroy(rb);
    return ok;
}

#ifdef RINGBUF_TEST_HOOKS

/* ---- Counter-overflow coverage.
 *
 * The internal counters are monotonic and only mapped onto slots via a mask, so
 * they eventually overflow SIZE_MAX. Reaching that organically would take 2^64
 * reservations, so this parks the counters just below the boundary with
 * ringbuf_zcopy_test_seed_counters() and then runs across it.
 *
 * test_ringbuf_zcopy.c already covers the boundary single-producer and with a
 * filler pool; the one case here is the multi-producer counterpart, which puts
 * the wrapped counters and the producer-to-producer hand-off under stress at
 * the same time -- write_idx wraps while being passed between four threads
 * through the push mutex.
 * ---- */

#define SEED_BASE (SIZE_MAX - 2)

/**
 * run_wrap_mp_parallel_fill_test - several producers across the SIZE_MAX
 *	counter boundary
 *
 * The stamps hold absolute ticket numbers and cross the boundary alongside the
 * counters, so this is also what would catch a stamp comparison written as an
 * absolute magnitude rather than a signed difference.
 *
 * Return: true on pass.
 */
static bool run_wrap_mp_parallel_fill_test(void)
{
    struct ringbuf_zcopy *rb =
        ringbuf_zcopy_create(sizeof(struct message), 4, true);
    if (!rb) {
        fprintf(stderr,
                "wrap_mpsc_parallel_fill: ringbuf_zcopy_create failed\n");
        return false;
    }
    ringbuf_zcopy_test_seed_counters(rb, SEED_BASE);
    bool ok = run_mp_on(rb, mp_reserve_producer_main, 200000u, false,
                        "wrap_mpsc_parallel_fill");
    ringbuf_zcopy_destroy(rb);
    return ok;
}

#endif /* RINGBUF_TEST_HOOKS */

/**
 * main - run all five cases unconditionally
 *
 * There is no filter flag; the wrap_* case is present only under
 * -DRINGBUF_TEST_HOOKS. The single-producer contract is covered by
 * test_ringbuf_zcopy.c and deliberately not repeated here.
 *
 * Return: 0 if every case passed, 1 otherwise.
 */
int main(void)
{
    bool ok = true;

    ok &= run_mp_push_test();
    ok &= run_mp_parallel_fill_test();
    ok &= run_mp_parallel_fill_overwrite_test();
    ok &= run_peer_unblocks_reserve_test();

#ifdef RINGBUF_TEST_HOOKS
    ok &= run_wrap_mp_parallel_fill_test();
#else
    printf("counter-overflow test SKIPPED (build with -DRINGBUF_TEST_HOOKS)\n");
#endif

    if (!ok) {
        printf("FAIL\n");
        return 1;
    }

    printf("PASS\n");
    return 0;
}
