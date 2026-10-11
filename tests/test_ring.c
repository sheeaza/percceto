/* Ring mechanics in isolation -- no protobuf, no files, no pftrace.
 *
 * The properties worth proving are the ones that would be invisible in a
 * passing end-to-end run: that the mirror really makes a straddling
 * record contiguous, that concurrent producers never corrupt each other,
 * and that a dropped event is counted rather than silently lost. */

#include <inttypes.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pf_ring.h"

static int failures;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
            failures++;                                   \
        }                                                 \
    } while (0)

/* Test records: pf_ring's tag, an 8-byte sequence number, then a
 * byte-pattern payload derived from that number so corruption is
 * detectable rather than merely suspected.
 *
 * Fixed size, like every record the ring now carries: the payload fills
 * whatever the stride leaves after the tag and the sequence number. */
struct t_rec {
    struct pf_rec_hdr hdr;
    uint64_t seq;
    uint8_t payload[];
};

#define T_SEQ offsetof(struct t_rec, seq)
#define T_PAYLOAD offsetof(struct t_rec, payload)
#define T_PAYLOAD_BYTES (PF_REC_BYTES - T_PAYLOAD)

static void fill_rec(uint8_t *rec, uint64_t seq)
{
    memcpy(rec + T_SEQ, &seq, 8);
    for (uint32_t i = 0; i < T_PAYLOAD_BYTES; i++)
        rec[T_PAYLOAD + i] = (uint8_t)(seq + i);
}

static bool check_rec(const uint8_t *rec, uint64_t *seq_out)
{
    uint64_t seq;
    memcpy(&seq, rec + T_SEQ, 8);
    *seq_out = seq;
    for (uint32_t i = 0; i < T_PAYLOAD_BYTES; i++) {
        uint8_t want = (uint8_t)(seq + i);
        if (rec[T_PAYLOAD + i] != want)
            return false;
    }
    return true;
}

/* --- 1. Geometry ------------------------------------------------------- */

static void test_geometry(void)
{
    printf("geometry:\n");

    /* A non-power-of-two request rounds up; a tiny one hits the floor. */
    struct pf_ring r;
    CHECK(pf_ring_init(&r, 100 * 1024, true) == 0, "init 100KiB");
    CHECK(r.cap == 128 * 1024, "cap rounded to 128KiB, got %zu", r.cap);
    CHECK((r.cap & r.mask) == 0, "cap is a power of two");
    pf_ring_destroy(&r);

    CHECK(pf_ring_init(&r, 1, true) == 0, "init below floor");
    CHECK(r.cap >= PF_RING_MIN_BYTES, "floor applied, got %zu", r.cap);
    pf_ring_destroy(&r);

    /* The two mappings must be the same memory: a write through the
     * mirror has to be visible at the front of the buffer. */
    CHECK(pf_ring_init(&r, PF_RING_MIN_BYTES, true) == 0, "init");
    r.base[0] = 0xAB;
    CHECK(r.base[r.cap] == 0xAB, "mirror aliases the front of the buffer");
    r.base[r.cap + 7] = 0xCD;
    CHECK(r.base[7] == 0xCD, "front aliases the mirror");
    pf_ring_destroy(&r);
    printf("  ok\n");
}

/* --- 2. Straddling the end -------------------------------------------- */

/* The case the mirror exists for. The stride does not divide cap --
 * gcd(40, 2^n) is 8 -- so as head advances the in-buffer offset visits
 * every multiple of 8, and a record eventually starts close enough to the
 * end to cross it. This finds that record, writes it with one memcpy, and
 * reads it back as one contiguous span.
 *
 * Trimming as it fills mirrors what the drain thread does in snapshot
 * mode: an untrimmed ring is exactly full by the time head reaches the
 * end, so the straddling record's tail would legitimately overwrite the
 * record at offset 0 and the reader would be right to refuse it. */
static void test_straddle(void)
{
    printf("straddle:\n");

    struct pf_ring r;
    CHECK(pf_ring_init(&r, PF_RING_MIN_BYTES, true) == 0, "init");

    /* Advance until the next record would cross cap, i.e. starts in the
     * last PF_REC_BYTES of the buffer without landing exactly on the end.
     * Reachable because the stride is coprime to cap beyond a factor 8. */
    const uint64_t marker = 0xFEEDFACEu;
    struct pf_rec_res res;
    size_t straddle_off;
    uint64_t guard = 0;
    while (true) {
        uint64_t head = atomic_load_explicit(&r.head, memory_order_relaxed);
        size_t off = (size_t)(head & r.mask);
        if (off > r.cap - PF_REC_BYTES)
            break;
        CHECK(guard++ < r.cap, "straddling offset is reachable");
        if (guard > r.cap)
            break;
        res = pf_ring_reserve(&r);
        CHECK(res.rec != NULL, "reserve filler");
        fill_rec((uint8_t *)res.rec, head);
        pf_ring_submit(&r, res);
        pf_ring_trim(&r, r.cap / 2);
    }

    res = pf_ring_reserve(&r);
    CHECK(res.rec != NULL, "reserve straddling record");
    /* Offset within the buffer, which is all this check needs; the
     * absolute position lives in the reservation's tag. */
    straddle_off = (size_t)((uint8_t *)res.rec - r.base);
    CHECK(straddle_off > r.cap - PF_REC_BYTES,
          "record starts in the last stride, at %zu of %zu", straddle_off,
          r.cap);
    fill_rec((uint8_t *)res.rec, marker);
    pf_ring_submit(&r, res);

    /* The part past cap must be visible at the front of the buffer. */
    size_t past = straddle_off + PF_REC_BYTES - r.cap;
    CHECK(memcmp(r.base + r.cap, r.base, past) == 0,
          "the %zu bytes past cap alias the front", past);

    /* And the reader must see it as one undivided record. */
    struct pf_ring_reader rd;
    pf_ring_read_begin(&r, &rd);
    bool found = false;
    const uint8_t *got;
    while ((got = pf_ring_next(&r, &rd)) != NULL) {
        uint64_t seq;
        CHECK(check_rec(got, &seq), "record intact");
        if (seq == marker)
            found = true;
    }
    CHECK(found, "straddling record was read back");

    pf_ring_destroy(&r);
    printf("  ok\n");
}

/* --- 3. Concurrent producers ------------------------------------------ */

#define NTHREADS 8
#ifndef PER_THREAD
#define PER_THREAD 100000
#endif

struct producer_arg {
    struct pf_ring *ring;
    unsigned id;
    uint64_t published;
};

static void *producer_fn(void *p)
{
    struct producer_arg *a = p;
    for (unsigned i = 0; i < PER_THREAD; i++) {
        struct pf_rec_res res = pf_ring_reserve(a->ring);
        if (!res.rec)
            continue; /* dropping-mode drop; counted by the ring */
        /* Sequence encodes which producer wrote it, so a torn record
         * shows up as a payload mismatch rather than going unnoticed. */
        fill_rec((uint8_t *)res.rec, ((uint64_t)a->id << 32) | i);
        pf_ring_submit(a->ring, res);
        a->published++;
    }
    return NULL;
}

/* Dropping (continuous) mode: a concurrent reader consumes while 8
 * producers write. Nothing may be corrupt, and every attempted event must
 * be either published or counted as dropped -- never silently lost. */
static void test_concurrent_dropping(void)
{
    printf("concurrent, dropping (%d threads x %d):\n", NTHREADS, PER_THREAD);

    struct pf_ring r;
    /* Deliberately small, so the reader must keep up and drops are
     * plausible -- that is the path being tested. */
    CHECK(pf_ring_init(&r, 256 * 1024, false) == 0, "init");

    pthread_t th[NTHREADS];
    struct producer_arg args[NTHREADS];
    for (unsigned i = 0; i < NTHREADS; i++) {
        args[i] = (struct producer_arg){ .ring = &r, .id = i, .published = 0 };
        CHECK(pthread_create(&th[i], NULL, producer_fn, &args[i]) == 0,
              "spawn producer %u", i);
    }

    /* Drain concurrently until the producers are done and the ring is
     * empty. Counts and validates every record it sees. */
    uint64_t consumed = 0;
    bool corrupt = false;
    bool joined[NTHREADS] = { false };
    unsigned n_joined = 0;
    while (true) {
        struct pf_ring_reader rd;
        pf_ring_read_begin(&r, &rd);
        const uint8_t *got;
        uint64_t before = consumed;
        while ((got = pf_ring_next(&r, &rd)) != NULL) {
            uint64_t seq;
            if (!check_rec(got, &seq))
                corrupt = true;
            consumed++;
        }
        pf_ring_read_commit(&r, &rd);

        /* Reap each producer exactly once; tryjoin on an already-joined
         * thread is undefined. */
        for (unsigned i = 0; i < NTHREADS; i++) {
            if (!joined[i] && pthread_tryjoin_np(th[i], NULL) == 0) {
                joined[i] = true;
                n_joined++;
            }
        }

        /* Only stop once every producer is reaped and a full pass found
         * nothing new, so the tail of the ring is never left behind. */
        if (n_joined == NTHREADS && consumed == before)
            break;
    }

    CHECK(!corrupt, "no record was torn or interleaved");

    uint64_t published =
        atomic_load_explicit(&r.published, memory_order_relaxed);
    uint64_t dropped = atomic_load_explicit(&r.dropped, memory_order_relaxed);
    uint64_t attempted = (uint64_t)NTHREADS * PER_THREAD;

    printf("  published=%" PRIu64 " dropped=%" PRIu64 " consumed=%" PRIu64
           " attempted=%" PRIu64 "\n",
           published, dropped, consumed, attempted);

    CHECK(published + dropped == attempted,
          "every event accounted for: %" PRIu64 " + %" PRIu64 " != %" PRIu64,
          published, dropped, attempted);
    CHECK(consumed == published,
          "reader saw every published record: %" PRIu64 " != %" PRIu64,
          consumed, published);
    CHECK(atomic_load_explicit(&r.resyncs, memory_order_relaxed) == 0,
          "dropping mode never resyncs");

    pf_ring_destroy(&r);
    printf("  ok\n");
}

/* Overwriting (snapshot) mode: producers overwrite freely. Records must
 * still never be torn, and the ring must not lose its boundaries. */
static void test_concurrent_overwriting(void)
{
    printf("concurrent, overwriting (%d threads x %d):\n", NTHREADS,
           PER_THREAD);

    struct pf_ring r;
    CHECK(pf_ring_init(&r, 256 * 1024, true) == 0, "init");

    pthread_t th[NTHREADS];
    struct producer_arg args[NTHREADS];
    for (unsigned i = 0; i < NTHREADS; i++) {
        args[i] = (struct producer_arg){ .ring = &r, .id = i, .published = 0 };
        CHECK(pthread_create(&th[i], NULL, producer_fn, &args[i]) == 0,
              "spawn producer %u", i);
    }

    /* Trim continuously, the way the drain thread does in snapshot mode,
     * until every producer has finished. A fixed number of rounds would
     * let producers lap the trim and force resyncs -- which is a real
     * property of snapshot mode, but not what this test is pinning down. */
    bool joined[NTHREADS] = { false };
    unsigned n_joined = 0;
    while (n_joined < NTHREADS) {
        pf_ring_trim(&r, r.cap / 4 * 3);
        for (unsigned i = 0; i < NTHREADS; i++) {
            if (!joined[i] && pthread_tryjoin_np(th[i], NULL) == 0) {
                joined[i] = true;
                n_joined++;
            }
        }
    }

    CHECK(atomic_load_explicit(&r.dropped, memory_order_relaxed) == 0,
          "overwriting mode never drops");
    CHECK(atomic_load_explicit(&r.published, memory_order_relaxed) ==
              (uint64_t)NTHREADS * PER_THREAD,
          "every event published");

    /* Whatever survives must be well formed: the tag has to keep the
     * reader out of records that were overwritten under it. */
    pf_ring_trim(&r, r.cap / 2);
    struct pf_ring_reader rd;
    pf_ring_read_begin(&r, &rd);
    const uint8_t *got;
    uint64_t seen = 0, torn = 0;
    while ((got = pf_ring_next(&r, &rd)) != NULL) {
        uint64_t pos = rd.pos - PF_REC_BYTES, seq;
        bool ok = check_rec(got, &seq);
        /* Only hold a record to account if it was not overwritten while
         * being examined -- the same seqlock the drainer uses. */
        if (pf_ring_still_valid(&r, pos)) {
            seen++;
            if (!ok)
                torn++;
        }
    }
    printf("  survivors=%" PRIu64 " torn=%" PRIu64 "\n", seen, torn);
    CHECK(torn == 0, "no surviving record was torn");
    CHECK(seen > 0, "some records survived the wrap");

    pf_ring_destroy(&r);
    printf("  ok\n");
}

/* --- 4. Drop accounting ----------------------------------------------- */

/* A ring that is never drained must drop, and must say so exactly. */
static void test_drop_accounting(void)
{
    printf("drop accounting:\n");

    struct pf_ring r;
    CHECK(pf_ring_init(&r, PF_RING_MIN_BYTES, false) == 0, "init");

    uint64_t ok = 0;
    for (unsigned i = 0; i < 100000; i++) {
        struct pf_rec_res res = pf_ring_reserve(&r);
        if (!res.rec)
            continue;
        fill_rec((uint8_t *)res.rec, i);
        pf_ring_submit(&r, res);
        ok++;
    }

    uint64_t dropped = atomic_load_explicit(&r.dropped, memory_order_relaxed);
    printf("  accepted=%" PRIu64 " dropped=%" PRIu64 "\n", ok, dropped);
    CHECK(ok == r.cap / PF_REC_BYTES,
          "accepted exactly a ringful: %" PRIu64 " vs %zu", ok,
          r.cap / PF_REC_BYTES);
    CHECK(ok + dropped == 100000, "all attempts accounted for");

    /* head must not have advanced past what was actually published: a
     * refused reservation has to leave no hole. */
    uint64_t head = atomic_load_explicit(&r.head, memory_order_relaxed);
    CHECK(head == ok * PF_REC_BYTES,
          "no gap left by refused reservations: head=%" PRIu64 " want=%" PRIu64,
          head, ok * PF_REC_BYTES);

    /* Draining frees space, so claims must succeed again. */
    struct pf_ring_reader rd;
    pf_ring_read_begin(&r, &rd);
    while (pf_ring_next(&r, &rd) != NULL) {
    }
    pf_ring_read_commit(&r, &rd);

    CHECK(pf_ring_reserve(&r).rec != NULL, "reserve succeeds after drain");

    pf_ring_destroy(&r);
    printf("  ok\n");
}

/* --- 5. One producer, many wraps -------------------------------------- */

/* The uncontended path, which is also the deterministic one: with a single
 * thread and no reader racing it, no resync may happen at all. Snapshot
 * mode can still resync under contention, when a producer laps a record
 * the reader is mid-copy, but nothing here should trigger it. */
static void test_uncontended_wraps(void)
{
    printf("one producer, many wraps:\n");

    struct pf_ring r;
    CHECK(pf_ring_init(&r, PF_RING_MIN_BYTES, true) == 0, "init");

    /* 5000 records is several times the 64KiB ring, so it wraps
     * repeatedly; trimming as we go is what keeps tail inside the window,
     * exactly as the snapshot-mode drain thread does. */
    for (unsigned i = 0; i < 5000; i++) {
        struct pf_rec_res res = pf_ring_reserve(&r);
        CHECK(res.rec != NULL, "reserve %u", i);
        fill_rec((uint8_t *)res.rec, i);
        pf_ring_submit(&r, res);
        if (i % 16 == 0)
            pf_ring_trim(&r, r.cap / 4 * 3);
    }

    pf_ring_trim(&r, r.cap / 2);
    struct pf_ring_reader rd;
    pf_ring_read_begin(&r, &rd);
    const uint8_t *got;
    uint64_t seen = 0;
    while ((got = pf_ring_next(&r, &rd)) != NULL) {
        uint64_t seq;
        CHECK(check_rec(got, &seq), "record intact");
        seen++;
    }
    CHECK(seen > 0, "records readable after repeated wraps");
    CHECK(atomic_load_explicit(&r.resyncs, memory_order_relaxed) == 0,
          "an uncontended producer never resyncs");

    pf_ring_destroy(&r);
    printf("  ok\n");
}

int main(void)
{
    test_geometry();
    test_straddle();
    test_drop_accounting();
    test_uncontended_wraps();
    test_concurrent_dropping();
    test_concurrent_overwriting();

    if (failures) {
        printf("\n%d CHECK(S) FAILED\n", failures);
        return 1;
    }
    printf("\nALL RING TESTS PASSED\n");
    return 0;
}
