#ifndef PF_RING_ZCOPY_H
#define PF_RING_ZCOPY_H

#include <pthread.h>

#include "ringbuf_zcopy.h"

/* The ringbuf_zcopy backend: slot-indexed, multi-producer under a mutex
 * this file supplies.
 *
 * Included by pf_ring.h, which is the interface this implements and the
 * file to read first. Never include this directly. Selected with
 * -DPF_RING_BACKEND_ZCOPY.
 *
 * THE ONE BACKEND THAT TAKES A LOCK ON THE HOT PATH, and the reason it is
 * worth benchmarking rather than simply rejected. ringbuf_zcopy keeps its
 * write index single-writer and its internals simpler than
 * ringbuf_mpsc's ticket CAS; the price is that producers must be
 * serialized by a mutex the CALLER owns -- see "SEVERAL PRODUCERS" in
 * ringbuf_zcopy.h. Whether simpler internals plus a short lock beat a
 * lock-free CAS is exactly the open question; upstream's own benchmark
 * calls it a split decision that turns on contention.
 *
 * So this backend alone cannot honour pftrace.h's promise that an event
 * takes no lock. It is included to price that promise, not to replace the
 * ones that keep it.
 *
 * What the mutex does and does not cover is not a style choice. The lock
 * is held across @reserve ONLY: the fill and the submit happen with it
 * released, which is the whole point -- a mutex around the payload copy
 * would serialize exactly the work that was supposed to overlap. And it
 * must never be held while retrying a refused reserve, because a refusal
 * usually means a PEER holds an unsubmitted slot, and that peer needs this
 * same mutex to make progress. Retrying under it deadlocks. See
 * pf_ring_zcopy.c.
 *
 * Otherwise identical in shape to the ringbuf_mpsc backend: fixed slots,
 * a reserve that can fail in either mode, a reader that copies out, and
 * self-maintaining eviction that leaves trim with nothing to do. Read pf_ring_mpsc.h for the reasoning on all four; only the
 * synchronization differs.
 *
 * The buffer is held by POINTER, not embedded: struct ringbuf_zcopy ends
 * in a flexible array member, and C11 6.7.2.1p3 forbids such a struct as a
 * member of another. See src/VENDOR.md.
 */

struct pf_ring {
    struct ringbuf_zcopy *rb;

    /* Read back from ringbuf_zcopy_capacity() at init rather than computed
     * from the requested size, which the library rounds up. Cached here so
     * pf_ring_capacity_bytes costs no pointer hop. */
    size_t cap_bytes;

    bool overwrite;

    /* pftrace's counters, not the library's -- see pf_ring_mpsc.h, where
     * the same three serve the same purposes. @dropped counts refused
     * reserves only, never the records overwrite mode intentionally
     * evicts. */
    _Atomic uint64_t published;
    _Atomic uint64_t dropped;
    /* Written by the draining thread only, under pf_trace.out_lock. */
    _Atomic uint64_t consumed;
    _Atomic uint64_t retries;

    /* Where pf_ring_next retires a record to. Owned by the draining
     * thread, which pf_trace.out_lock serializes.
     *
     * Aligned because the caller overlays a struct on it. */
    _Alignas(PF_REC_ALIGN) uint8_t pop_buf[PF_PAYLOAD_BYTES];

    /* The caller-supplied mutex ringbuf_zcopy's multi-producer
     * arrangement requires. OURS, not the library's: struct
     * ringbuf_zcopy has a visible `lock` member since upstream opened the
     * struct up, and reaching for that one instead would be wrong twice
     * over -- it is uninitialized in lossless mode, and in overwrite mode
     * it is the lock reconciling the library's own two writers of
     * read_idx, not a producer lock. ringbuf_zcopy.h warns about exactly
     * this.
     *
     * Must be a real lock, not a hand-rolled spinlock over relaxed
     * atomics: reserve() reads and writes the write index with relaxed
     * atomics, so one producer's store reaches the next producer's load
     * only through this mutex's release/acquire edges. A fake lock would
     * compile and mostly appear to work.
     *
     * _Alignas(64) to keep it off the line holding @rb and the counters,
     * which every producer reads on every event. Without it the lock's
     * cache line would ping between cores carrying the hot fields along
     * with it, and the measurement would include false sharing this
     * wrapper introduced rather than anything about the ring. */
    _Alignas(64) pthread_mutex_t push_lock;
};

/**
 * pf_ring_occupancy_bytes() - Bytes reserved but not yet released.
 * @r: Ring.
 * @need: Bytes the caller is about to reserve, included in the result so
 *      a watermark test sees the occupancy its own event will produce.
 *
 * Inline, for the reason given in pf_ring_mpsc.h: maybe_wake_drain calls
 * it on every event. Takes no lock -- it reads this wrapper's own
 * counters, not the library's indices, so it needs none.
 *
 * Return: Occupancy in bytes, counting @need.
 */
static inline uint64_t pf_ring_occupancy_bytes(const struct pf_ring *r,
                                               uint32_t need)
{
    struct pf_ring *m = (struct pf_ring *)r;
    uint64_t pub = atomic_load_explicit(&m->published, memory_order_relaxed);
    uint64_t con = atomic_load_explicit(&m->consumed, memory_order_relaxed);
    return (pub - con) * PF_PAYLOAD_BYTES + need;
}

#endif /* PF_RING_ZCOPY_H */
