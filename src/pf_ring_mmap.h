#ifndef PF_RING_MMAP_H
#define PF_RING_MMAP_H

/* The bespoke backend: a lockless fixed-stride record ring, in a
 * fixed-size mapping made at init and never resized.
 *
 * Included by pf_ring.h, which is the interface this implements and the
 * file to read first. Never include this directly.
 *
 * Every record is exactly %PF_REC_BYTES, which is what makes the rest of
 * this small: a record boundary is any multiple of the stride, so @tail
 * can be moved by arithmetic instead of by walking tags, there is no
 * length to validate, and a corrupt length is not expressible. Variable
 * length would buy a few bytes on the smallest events and cost all of
 * that.
 *
 * Wrap handling is delegated to the MMU rather than to branches: the same
 * physical pages are mapped twice, back to back, so a record that runs off
 * the end of the buffer continues into the mirror and the kernel folds
 * those bytes back to the front. Producers and the reader therefore treat
 * every record as contiguous -- no split memcpy, no padding records, and
 * no wasted tail bytes. Still needed despite the fixed stride, because the
 * stride does not divide a power-of-two @cap: gcd(40, 2^n) is 8, so the
 * in-buffer offset visits every multiple of 8 and records straddle the end
 * on most laps.
 *
 * This is the one backend that needs the mirror at all. The ringbuf
 * variants index fixed slots and never let a record cross a boundary, so
 * they have nothing to stitch together -- and correspondingly cannot hand
 * out a record larger than one slot.
 *
 * Positions (@head, @tail) are absolute monotonic byte counters, never
 * wrapped; a buffer offset is `pos & mask`. Keeping them absolute is what
 * makes the tag check below work, and it sidesteps the full-versus-empty
 * ambiguity of wrapped indices.
 *
 * Publication is per record, because producers reserve space in order but
 * finish copying out of order. Each record starts with an 8-byte atomic
 * tag, written release-last, holding `pos + PF_REC_BYTES` -- the absolute
 * position just past the record. A reader at absolute position p accepts a
 * record only if its tag equals `p + PF_REC_BYTES`, which rejects three
 * cases with one comparison:
 *   - never written: fresh mapping pages read as zero, and a published
 *     tag is always nonzero because it is biased past its own position;
 *   - still being written: the tag store has not landed yet, so what sits
 *     there is either zero or a lap-stale smaller value;
 *   - lapped: a newer record's tag is larger.
 *
 * The bias is load-bearing. A bare lap counter would read as 0 for a
 * never-written slot and 0 for every slot on lap 0, so the whole first
 * fill of the ring would be indistinguishable from empty -- a reader would
 * accept uninitialised bytes as a record.
 */

/**
 * struct pf_rec_hdr - The tag every record begins with.
 * @tag: `pos + PF_REC_BYTES`, as described above.
 *
 * Owned by this backend, not by the code storing records in it:
 * pf_ring_submit writes @tag release-last and the reader's check is a load
 * of it. A producer never sees it -- pf_ring_reserve hands back a pointer
 * past this header, so the payload a caller fills is %PF_PAYLOAD_BYTES
 * starting after it. That split is what makes publication atomic per
 * record.
 *
 * The reason %PF_PAYLOAD_BYTES excludes this: the ringbuf backends keep
 * readiness in a separate per-slot array, so they carry no equivalent and
 * must not be charged 8 bytes per event for one.
 */
struct pf_rec_hdr {
    _Atomic uint64_t tag;
};

/* The stride, and so also the largest (and only) record size: this
 * backend's own 8-byte tag plus the payload every backend carries.
 *
 * Every position the ring hands out is a multiple of this, which is what
 * the arithmetic in pf_ring_mmap.c assumes. */
#define PF_REC_BYTES (PF_PAYLOAD_BYTES + sizeof(struct pf_rec_hdr))
_Static_assert(PF_REC_BYTES % PF_REC_ALIGN == 0,
               "the stride keeps every tag 8-byte aligned");

struct pf_ring {
    uint8_t *base; /* first of two mappings of the same pages */
    size_t cap; /* power of two, hence also a page multiple */
    size_t mask; /* cap - 1 */

    /* Lap records the reader has not consumed, rather than refusing new
     * ones. Set for snapshot mode, where overwriting the oldest events is
     * the entire point, and clear for continuous mode, which drops
     * instead. */
    bool overwrite;

    _Atomic uint64_t head; /* next byte to reserve */
    _Atomic uint64_t tail; /* oldest byte the reader still needs */
    _Atomic uint64_t dropped;
    _Atomic uint64_t resyncs;
    _Atomic uint64_t published;
};

/**
 * pf_ring_occupancy_bytes() - Bytes reserved but not yet released.
 * @r: Ring.
 * @need: Bytes the caller is about to reserve, included in the result so
 *      a watermark test sees the occupancy its own event will produce.
 *
 * Inline, and the only accessor that is: pftrace.c's maybe_wake_drain
 * calls it on every event, so a function call here would show up in the
 * per-event cost this backend is being measured on. The other accessors
 * run once at open or once per dump.
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
    uint64_t head = atomic_load_explicit(&m->head, memory_order_relaxed);
    uint64_t tail = atomic_load_explicit(&m->tail, memory_order_relaxed);
    return head + need - tail;
}

#endif /* PF_RING_MMAP_H */
