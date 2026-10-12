#ifndef PF_RING_MPSC_H
#define PF_RING_MPSC_H

#include "ringbuf_mpsc.h"

/* The ringbuf_mpsc backend: lock-free, multi-producer, slot-indexed.
 *
 * Included by pf_ring.h, which is the interface this implements and the
 * file to read first. Never include this directly. Selected with
 * -DPF_RING_BACKEND_MPSC.
 *
 * The like-for-like alternative to the mmap backend: producers claim with
 * a ticket CAS, so any number of threads may reserve concurrently with no
 * caller-held lock, and pftrace.h's promise that an event takes no lock
 * holds unchanged. What differs is the storage model -- fixed slots
 * indexed by ticket, rather than a byte stream with a double mapping --
 * and three consequences follow from it:
 *
 *   - A reserve can fail in EITHER mode. The mmap backend's overwrite
 *     reserve is an unconditional fetch-add and cannot; here a slot whose
 *     previous-lap reservation has not been submitted is refused, because
 *     a filler may still be writing those bytes. Rare with pftrace's
 *     four-store fill, but pf_ring_reserve retries before giving up.
 *   - The reader copies out rather than walking in place. ringbuf_mpsc_pop
 *     retires a record into a caller buffer, which is strictly safer --
 *     nothing can lap a record the reader already holds -- but it costs a
 *     memcpy per event that the mmap reader avoids. That trade is one of
 *     the things the benchmark measures.
 *   - trim and resync have nothing to do. Eviction advances the library's
 *     own read index, so the snapshot window maintains itself and the
 *     reader's starting point is never a position already recycled.
 *     &pf_stats.resyncs is therefore structurally zero.
 *
 * The buffer is held by POINTER, not embedded: struct ringbuf_mpsc ends in
 * a flexible array member, and C11 6.7.2.1p3 forbids such a struct as a
 * member of another. See src/VENDOR.md for what embedding would have cost.
 */

struct pf_ring {
    struct ringbuf_mpsc *rb;

    /* Read back from ringbuf_mpsc_capacity() at init rather than computed
     * from the requested size, which the library rounds up. Cached here so
     * pf_ring_capacity_bytes costs no pointer hop. */
    size_t cap_bytes;

    bool overwrite;

    /* pftrace's counters, not the library's.
     *
     * @published and @consumed exist because occupancy is on the hot path:
     * maybe_wake_drain needs it per event, and deriving it from the
     * library's internals would mean reading write_idx and read_idx
     * through @rb on every event. Two counters here are cheaper and let
     * pf_ring_occupancy_bytes inline to arithmetic on this struct.
     *
     * @dropped counts only reserves this backend REFUSED after exhausting
     * its retries -- never the records overwrite mode intentionally
     * evicts. pftrace.h defines &pf_stats.dropped as meaning a gap in the
     * output, and an eviction in snapshot mode is the mode working as
     * intended. ringbuf_mpsc_dropped_count() holds the eviction tally if
     * a caller wants it; nothing in pftrace reads it. */
    _Atomic uint64_t published;
    _Atomic uint64_t dropped;
    /* Written by the draining thread only, under pf_trace.out_lock. */
    _Atomic uint64_t consumed;

    /* Reserve attempts that were refused and retried. Diagnostic: the
     * figure the benchmark reports to show what the slot-indexed model
     * costs under contention, which the mmap backend has no equivalent
     * of. */
    _Atomic uint64_t retries;

    /* Where pf_ring_next retires a record to. Owned by the draining
     * thread, which pf_trace.out_lock serializes, so one buffer suffices
     * and the reader allocates nothing.
     *
     * Aligned because the caller overlays a struct on it -- the same
     * reason pf_trace.rec_copy is aligned. */
    _Alignas(PF_REC_ALIGN) uint8_t pop_buf[PF_PAYLOAD_BYTES];
};

/**
 * pf_ring_occupancy_bytes() - Bytes reserved but not yet released.
 * @r: Ring.
 * @need: Bytes the caller is about to reserve, included in the result so
 *      a watermark test sees the occupancy its own event will produce.
 *
 * Inline, and the only accessor that is: pftrace.c's maybe_wake_drain
 * calls it on every event, so a function call here would show up in the
 * per-event cost this backend is being measured on.
 *
 * Relaxed loads. The result is a heuristic -- it decides whether to nudge
 * the drain thread -- and nothing is ordered against it. A subtraction of
 * two monotonic counters rather than a comparison, so it stays correct
 * across the point where they wrap.
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

#endif /* PF_RING_MPSC_H */
