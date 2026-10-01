/* Ring mechanics in isolation -- no protobuf, no files, no pftrace.
 *
 * The properties worth proving are the ones that would be invisible in a
 * passing end-to-end run: that the mirror really makes a straddling
 * record contiguous, that concurrent producers never corrupt each other,
 * and that a dropped event is counted rather than silently lost. */

#include "pf_ring.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Test records: an 8-byte tag, an 8-byte sequence number, then a
 * byte-pattern payload derived from that number so corruption is
 * detectable rather than merely suspected. */
#define T_SEQ 8
#define T_PAYLOAD 16

static uint32_t rec_size(uint32_t payload)
{
    return (uint32_t)((T_PAYLOAD + payload + 7u) & ~7u);
}

static void fill_rec(uint8_t *rec, uint64_t seq, uint32_t payload)
{
    memcpy(rec + T_SEQ, &seq, 8);
    for (uint32_t i = 0; i < payload; i++)
        rec[T_PAYLOAD + i] = (uint8_t)(seq + i);
}

static bool check_rec(const uint8_t *rec, uint32_t len, uint64_t *seq_out)
{
    uint64_t seq;
    memcpy(&seq, rec + T_SEQ, 8);
    *seq_out = seq;
    /* len is rounded up, so only the bytes we know were written are
     * checked; trailing pad bytes are unspecified. */
    for (uint32_t i = 0; i + T_PAYLOAD < len; i++) {
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
    CHECK(pf_ring_init(&r, 100 * 1024, false, false) == 0, "init 100KiB");
    CHECK(r.cap == 128 * 1024, "cap rounded to 128KiB, got %zu", r.cap);
    CHECK((r.cap & r.mask) == 0, "cap is a power of two");
    CHECK(((size_t)1 << r.log2cap) == r.cap, "log2cap matches cap");
    pf_ring_destroy(&r);

    CHECK(pf_ring_init(&r, 1, false, false) == 0, "init below floor");
    CHECK(r.cap >= PF_RING_MIN_BYTES, "floor applied, got %zu", r.cap);
    pf_ring_destroy(&r);

    /* The two mappings must be the same memory: a write through the
     * mirror has to be visible at the front of the buffer. */
    CHECK(pf_ring_init(&r, PF_RING_MIN_BYTES, false, false) == 0, "init");
    r.base[0] = 0xAB;
    CHECK(r.base[r.cap] == 0xAB, "mirror aliases the front of the buffer");
    r.base[r.cap + 7] = 0xCD;
    CHECK(r.base[7] == 0xCD, "front aliases the mirror");
    pf_ring_destroy(&r);
    printf("  ok\n");
}

/* --- 2. Straddling the end -------------------------------------------- */

/* The case the mirror exists for. Positions a record so it crosses cap,
 * writes it with one memcpy, and reads it back as one contiguous span.
 *
 * Trimming as it fills mirrors what the drain thread does in snapshot
 * mode: an untrimmed ring is exactly full by the time head reaches the
 * end, so the straddling record's tail would legitimately overwrite the
 * record at offset 0 and the reader would be right to refuse it. */
static void test_straddle(void)
{
    printf("straddle:\n");

    struct pf_ring r;
    CHECK(pf_ring_init(&r, PF_RING_MIN_BYTES, false, false) == 0, "init");

    /* Walk head to 32 bytes before the end, then write a 64-byte record
     * so 32 bytes land past cap. */
    const uint32_t big = 64;
    uint64_t off;
    uint8_t *rec;
    while (true) {
        uint64_t head = atomic_load_explicit(&r.head, memory_order_relaxed);
        if (head % r.cap == r.cap - 32)
            break;
        rec = pf_ring_claim(&r, 32, &off);
        CHECK(rec != NULL, "claim filler");
        fill_rec(rec, off, 32 - T_PAYLOAD);
        pf_ring_publish(&r, off, 32);
        pf_ring_trim(&r, r.cap / 2);
    }

    uint64_t straddle_off;
    rec = pf_ring_claim(&r, big, &straddle_off);
    CHECK(rec != NULL, "claim straddling record");
    CHECK(straddle_off % r.cap == r.cap - 32, "record starts 32B before end");
    fill_rec(rec, 0xFEEDFACEu, big - T_PAYLOAD);
    pf_ring_publish(&r, straddle_off, big);

    /* The tail half must be visible at the front of the buffer. */
    CHECK(memcmp(r.base + (straddle_off & r.mask) + 32, r.base, 32) == 0,
          "bytes past cap alias the front");

    /* And the reader must see it as one undivided record. */
    struct pf_ring_reader rd;
    pf_ring_read_begin(&r, &rd);
    bool found = false;
    const uint8_t *got;
    uint32_t len;
    while ((got = pf_ring_next(&r, &rd, &len)) != NULL) {
        uint64_t seq;
        CHECK(check_rec(got, len, &seq), "record intact at len %u", len);
        if (seq == 0xFEEDFACEu) {
            found = true;
            CHECK(len == big, "straddling record len %u, want %u", len, big);
        }
    }
    CHECK(found, "straddling record was read back");

    pf_ring_destroy(&r);
    printf("  ok\n");
}

/* --- 3. Concurrent producers ------------------------------------------ */

#define NTHREADS 8
#define PER_THREAD 100000

struct producer_arg {
    struct pf_ring *ring;
    unsigned id;
    uint64_t published;
};

static void *producer_fn(void *p)
{
    struct producer_arg *a = p;
    for (unsigned i = 0; i < PER_THREAD; i++) {
        /* Vary the size so records are genuinely variable-length and
         * straddles happen at many different offsets. */
        uint32_t payload = (uint32_t)((a->id * 13u + i * 7u) % 200u);
        uint32_t need = rec_size(payload);
        uint64_t off;
        uint8_t *rec = pf_ring_claim(a->ring, need, &off);
        if (!rec)
            continue; /* bounded mode drop; counted by the ring */
        /* Sequence encodes which producer wrote it, so a torn record
         * shows up as a payload mismatch rather than going unnoticed. */
        fill_rec(rec, ((uint64_t)a->id << 32) | i, need - T_PAYLOAD);
        pf_ring_publish(a->ring, off, need);
        a->published++;
    }
    return NULL;
}

/* Bounded (continuous) mode: a concurrent reader consumes while 8
 * producers write. Nothing may be corrupt, and every attempted event must
 * be either published or counted as dropped -- never silently lost. */
static void test_concurrent_bounded(void)
{
    printf("concurrent, bounded (%d threads x %d):\n", NTHREADS, PER_THREAD);

    struct pf_ring r;
    /* Deliberately small, so the reader must keep up and drops are
     * plausible -- that is the path being tested. */
    CHECK(pf_ring_init(&r, 256 * 1024, true, false) == 0, "init");

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
        uint32_t len;
        uint64_t before = consumed;
        while ((got = pf_ring_next(&r, &rd, &len)) != NULL) {
            uint64_t seq;
            if (!check_rec(got, len, &seq))
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
          "bounded mode never resyncs");

    pf_ring_destroy(&r);
    printf("  ok\n");
}

/* Unbounded (snapshot) mode: producers overwrite freely. Records must
 * still never be torn, and the ring must not lose its boundaries. */
static void test_concurrent_unbounded(void)
{
    printf("concurrent, unbounded (%d threads x %d):\n", NTHREADS, PER_THREAD);

    struct pf_ring r;
    CHECK(pf_ring_init(&r, 256 * 1024, false, false) == 0, "init");

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
          "unbounded mode never drops");
    CHECK(atomic_load_explicit(&r.published, memory_order_relaxed) ==
              (uint64_t)NTHREADS * PER_THREAD,
          "every event published");

    /* Whatever survives must be well formed: the lap tag has to keep the
     * reader out of records that were overwritten under it. */
    pf_ring_trim(&r, r.cap / 2);
    struct pf_ring_reader rd;
    pf_ring_read_begin(&r, &rd);
    const uint8_t *got;
    uint32_t len;
    uint64_t seen = 0, torn = 0;
    while ((got = pf_ring_next(&r, &rd, &len)) != NULL) {
        uint64_t pos = rd.pos - len, seq;
        bool ok = check_rec(got, len, &seq);
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
    CHECK(pf_ring_init(&r, PF_RING_MIN_BYTES, true, true) == 0, "init");

    const uint32_t need = 64;
    uint64_t ok = 0;
    for (unsigned i = 0; i < 100000; i++) {
        uint64_t off;
        uint8_t *rec = pf_ring_claim(&r, need, &off);
        if (!rec)
            continue;
        fill_rec(rec, i, need - T_PAYLOAD);
        pf_ring_publish(&r, off, need);
        ok++;
    }

    uint64_t dropped = atomic_load_explicit(&r.dropped, memory_order_relaxed);
    printf("  accepted=%" PRIu64 " dropped=%" PRIu64 "\n", ok, dropped);
    CHECK(ok == r.cap / need, "accepted exactly a ringful: %" PRIu64 " vs %zu",
          ok, r.cap / need);
    CHECK(ok + dropped == 100000, "all attempts accounted for");

    /* head must not have advanced past what was actually published: a
     * refused claim has to leave no hole. */
    uint64_t head = atomic_load_explicit(&r.head, memory_order_relaxed);
    CHECK(head == ok * need,
          "no gap left by refused claims: head=%" PRIu64 " want=%" PRIu64, head,
          ok * need);

    /* Draining frees space, so claims must succeed again. */
    struct pf_ring_reader rd;
    pf_ring_read_begin(&r, &rd);
    uint32_t len;
    while (pf_ring_next(&r, &rd, &len) != NULL) {
    }
    pf_ring_read_commit(&r, &rd);

    uint64_t off;
    CHECK(pf_ring_claim(&r, need, &off) != NULL, "claim succeeds after drain");

    pf_ring_destroy(&r);
    printf("  ok\n");
}

/* --- 5. Single-producer fast path ------------------------------------- */

static void test_single_producer(void)
{
    printf("single producer:\n");

    struct pf_ring r;
    CHECK(pf_ring_init(&r, PF_RING_MIN_BYTES, false, true) == 0, "init");

    /* 5000 records of up to ~216B is several times the 64KiB ring, so it
     * wraps repeatedly; trimming as we go is what keeps tail on a live
     * boundary, exactly as the snapshot-mode drain thread does. */
    for (unsigned i = 0; i < 5000; i++) {
        uint32_t need = rec_size(i % 100u);
        uint64_t off;
        uint8_t *rec = pf_ring_claim(&r, need, &off);
        CHECK(rec != NULL, "claim %u", i);
        fill_rec(rec, i, need - T_PAYLOAD);
        pf_ring_publish(&r, off, need);
        if (i % 16 == 0)
            pf_ring_trim(&r, r.cap / 4 * 3);
    }

    pf_ring_trim(&r, r.cap / 2);
    struct pf_ring_reader rd;
    pf_ring_read_begin(&r, &rd);
    const uint8_t *got;
    uint32_t len;
    uint64_t seen = 0;
    while ((got = pf_ring_next(&r, &rd, &len)) != NULL) {
        uint64_t seq;
        CHECK(check_rec(got, len, &seq), "record intact");
        seen++;
    }
    CHECK(seen > 0, "records readable in single-producer mode");
    CHECK(atomic_load_explicit(&r.resyncs, memory_order_relaxed) == 0,
          "trimming often enough avoids any resync");

    pf_ring_destroy(&r);
    printf("  ok\n");
}

int main(void)
{
    test_geometry();
    test_straddle();
    test_drop_accounting();
    test_single_producer();
    test_concurrent_bounded();
    test_concurrent_unbounded();

    if (failures) {
        printf("\n%d CHECK(S) FAILED\n", failures);
        return 1;
    }
    printf("\nALL RING TESTS PASSED\n");
    return 0;
}
