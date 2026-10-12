/* Ring mechanics in isolation -- no protobuf, no files, no pftrace.
 *
 * The properties worth proving are the ones that would be invisible in a
 * passing end-to-end run: that the slot geometry and the publication tag
 * agree, that concurrent producers never corrupt each other, and that a
 * dropped event is counted rather than silently lost.
 *
 * Specific to the mmap backend, and deliberately so: the geometry case
 * below reads @base, @cap and @mask. The ringbuf backends keep their own
 * equivalents private and are covered by their upstream suites and by this
 * project's end-to-end tests rather than here. */

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

/* Test records: an 8-byte sequence number, then a byte-pattern payload
 * derived from that number so corruption is detectable rather than merely
 * suspected.
 *
 * Fixed size, like every record the ring now carries, and laid over the
 * payload the ring hands out -- the backend's own publication tag sits
 * outside it and is never visible here. */
struct t_rec {
    uint64_t seq;
    uint8_t payload[];
};

#define T_SEQ offsetof(struct t_rec, seq)
#define T_PAYLOAD offsetof(struct t_rec, payload)
#define T_PAYLOAD_BYTES (PF_PAYLOAD_BYTES - T_PAYLOAD)

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

    /* @cap counts SLOTS, and a request in bytes is read as payload bytes:
     * 100 KiB / 32 = 3200 slots, rounded up to 4096. */
    struct pf_ring r;
    CHECK(pf_ring_init(&r, 100 * 1024, true) == 0, "init 100KiB");
    CHECK(r.cap == 4096, "cap rounded to 4096 slots, got %zu", r.cap);
    CHECK((r.cap & r.mask) == 0, "cap is a power of two");
    /* The capacity reported back is payload bytes, which is the unit
     * pftrace derives its watermark and retained window from -- and the
     * same unit the ringbuf backends report, so the same request buys the
     * same event count on all three. */
    CHECK(pf_ring_capacity_bytes(&r) == 4096 * PF_PAYLOAD_BYTES,
          "capacity is payload bytes, got %zu", pf_ring_capacity_bytes(&r));
    CHECK(pf_ring_capacity_bytes(&r) >= 100 * 1024,
          "capacity is a lower bound on the request");
    pf_ring_destroy(&r);

    CHECK(pf_ring_init(&r, 1, true) == 0, "init below floor");
    CHECK(pf_ring_capacity_bytes(&r) >= PF_RING_MIN_BYTES,
          "floor applied, got %zu", pf_ring_capacity_bytes(&r));
    pf_ring_destroy(&r);

    /* Slots must tile the allocation exactly: consecutive reservations are
     * one stride apart and never overlap, which is what replaced the
     * double mapping. */
    CHECK(pf_ring_init(&r, PF_RING_MIN_BYTES, true) == 0, "init");
    struct pf_rec_res a = pf_ring_reserve(&r);
    struct pf_rec_res b = pf_ring_reserve(&r);
    CHECK(a.rec && b.rec, "two reserves");
    CHECK(b.rec - a.rec == (ptrdiff_t)PF_REC_BYTES,
          "consecutive slots are one stride apart, got %td", b.rec - a.rec);
    CHECK(a.rec - sizeof(struct pf_rec_hdr) == r.base,
          "slot 0 starts at base");
    /* The last slot's payload must end exactly at the end of the
     * allocation: no mirror, so there is nothing past it to run into. */
    const uint8_t *last_end = r.base + (r.cap - 1) * PF_REC_BYTES +
                              sizeof(struct pf_rec_hdr) + PF_PAYLOAD_BYTES;
    CHECK(last_end == r.base + r.cap * PF_REC_BYTES,
          "the last slot ends at the end of the allocation");
    pf_ring_destroy(&r);
    printf("  ok\n");
}

/* --- 2. Wrapping ------------------------------------------------------- */

/* What the old straddle test guarded, now that there is no mirror to
 * exercise. When byte positions indexed a power-of-two buffer the stride
 * did not divide it -- gcd(40, 2^n) is 8 -- so a record eventually started
 * close enough to the end to cross it, and the double mapping existed to
 * keep those bytes contiguous. Indexing slots removes the case entirely:
 * position cap - 1 is the last slot and position cap is slot 0.
 *
 * So the property left to prove is that the wrap itself is clean -- a
 * record written at the last slot is read back intact, and the one written
 * at slot 0 immediately after it does not disturb it. A stride that was
 * mis-scaled anywhere (reserve, next, trim) would show up here as overlap.
 *
 * Trimming as it fills mirrors what the drain thread does in snapshot
 * mode: an untrimmed ring is exactly full by the time head reaches the
 * end, and the reader would be right to refuse the recycled records. */
static void test_wrap_boundary(void)
{
    printf("wrap boundary:\n");

    struct pf_ring r;
    CHECK(pf_ring_init(&r, PF_RING_MIN_BYTES, true) == 0, "init");

    /* Fill up to the last slot of lap 0, so the next two reservations are
     * the ones either side of the wrap. */
    const uint64_t last_marker = 0xFEEDFACEu;
    const uint64_t first_marker = 0xC0FFEEu;
    while (atomic_load_explicit(&r.head, memory_order_relaxed) < r.cap - 1) {
        struct pf_rec_res f = pf_ring_reserve(&r);
        CHECK(f.rec != NULL, "reserve filler");
        fill_rec(f.rec, atomic_load_explicit(&r.head, memory_order_relaxed));
        pf_ring_submit(&r, f);
        pf_ring_trim(&r, pf_ring_capacity_bytes(&r) / 2);
    }

    /* The last slot of the lap. */
    struct pf_rec_res last = pf_ring_reserve(&r);
    CHECK(last.rec != NULL, "reserve the last slot");
    CHECK((size_t)(last.rec - sizeof(struct pf_rec_hdr) - r.base) ==
              (r.cap - 1) * PF_REC_BYTES,
          "it is the last slot");
    fill_rec(last.rec, last_marker);
    pf_ring_submit(&r, last);

    /* And the first slot of the next lap, which must land back at base
     * without touching the record above. */
    struct pf_rec_res first = pf_ring_reserve(&r);
    CHECK(first.rec != NULL, "reserve across the wrap");
    CHECK(first.rec - sizeof(struct pf_rec_hdr) == r.base,
          "the wrap lands back at slot 0");
    fill_rec(first.rec, first_marker);
    pf_ring_submit(&r, first);

    /* The record at the last slot must have survived the wrap intact: if
     * anything were scaled wrong the write at slot 0 would have clipped
     * it. */
    uint64_t seq;
    CHECK(check_rec(last.rec, &seq) && seq == last_marker,
          "the last slot's record survived the wrap");

    /* And the reader must see both. */
    pf_ring_trim(&r, pf_ring_capacity_bytes(&r) / 2);
    struct pf_ring_reader rd;
    pf_ring_read_begin(&r, &rd);
    bool saw_last = false, saw_first = false;
    const uint8_t *got;
    while ((got = pf_ring_next(&r, &rd)) != NULL) {
        CHECK(check_rec(got, &seq), "record intact");
        if (seq == last_marker)
            saw_last = true;
        if (seq == first_marker)
            saw_first = true;
    }
    CHECK(saw_last, "the pre-wrap record was read back");
    CHECK(saw_first, "the post-wrap record was read back");

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
        fill_rec(res.rec, ((uint64_t)a->id << 32) | i);
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
        pf_ring_trim(&r, pf_ring_capacity_bytes(&r) / 4 * 3);
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
    pf_ring_trim(&r, pf_ring_capacity_bytes(&r) / 2);
    struct pf_ring_reader rd;
    pf_ring_read_begin(&r, &rd);
    const uint8_t *got;
    uint64_t seen = 0, torn = 0;
    while ((got = pf_ring_next(&r, &rd)) != NULL) {
        uint64_t seq;
        bool ok = check_rec(got, &seq);
        /* Only hold a record to account if it was not overwritten while
         * being examined -- the same seqlock the drainer uses. */
        if (pf_ring_still_valid(&r, rd.prev)) {
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
        fill_rec(res.rec, i);
        pf_ring_submit(&r, res);
        ok++;
    }

    uint64_t dropped = atomic_load_explicit(&r.dropped, memory_order_relaxed);
    printf("  accepted=%" PRIu64 " dropped=%" PRIu64 "\n", ok, dropped);
    CHECK(ok == r.cap, "accepted exactly a ringful: %" PRIu64 " vs %zu", ok,
          r.cap);
    CHECK(ok + dropped == 100000, "all attempts accounted for");

    /* head must not have advanced past what was actually published: a
     * refused reservation has to leave no hole. */
    uint64_t head = atomic_load_explicit(&r.head, memory_order_relaxed);
    CHECK(head == ok,
          "no gap left by refused reservations: head=%" PRIu64 " want=%" PRIu64,
          head, ok);

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
        fill_rec(res.rec, i);
        pf_ring_submit(&r, res);
        if (i % 16 == 0)
            pf_ring_trim(&r, pf_ring_capacity_bytes(&r) / 4 * 3);
    }

    pf_ring_trim(&r, pf_ring_capacity_bytes(&r) / 2);
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
    test_wrap_boundary();
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
