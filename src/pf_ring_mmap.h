#ifndef PF_RING_MMAP_H
#define PF_RING_MMAP_H

/* The bespoke backend: a lockless fixed-stride record ring, in a
 * fixed-size allocation made at init and never resized.
 *
 * Included by pf_ring.h, which is the interface this implements and the
 * file to read first. Never include this directly.
 *
 * Every record is exactly %PF_REC_BYTES, which is what makes the rest of
 * this small: there is no length to validate, a corrupt length is not
 * expressible, and @tail can be moved by arithmetic instead of by walking
 * tags. Variable length would buy a few bytes on the smallest events and
 * cost all of that.
 *
 * Positions (@head, @tail) count SLOTS, not bytes, and are absolute
 * monotonic counters, never wrapped; the slot they name is `pos & mask`
 * and its storage is at `base + (pos & mask) * PF_REC_BYTES`. Keeping them
 * absolute is what makes the tag check below work, and it sidesteps the
 * full-versus-empty ambiguity of wrapped indices.
 *
 * Counting slots is also what lets the storage be one plain allocation.
 * An earlier version of this backend counted bytes, which left the stride
 * not dividing a power-of-two @cap -- gcd(40, 2^n) is 8, so the in-buffer
 * offset visited every multiple of 8 and records straddled the end of the
 * buffer on most laps. That needed the same pages mapped twice back to
 * back, so the MMU would fold a straddling record's tail to the front and
 * every record could be treated as contiguous. Indexing slots makes every
 * position a boundary by construction, so the mirror had nothing left to
 * stitch: the memfd, the PROT_NONE reservation and the two MAP_FIXED
 * mappings all went away, and what replaced them is one aligned_alloc.
 * The alternative was padding the stride to 64 bytes, which divides @cap
 * but costs 1.6x the memory per event -- a snapshot ring would retain 62%
 * as many events for it.
 *
 * Publication is per record, because producers reserve slots in order but
 * finish copying out of order. Each record starts with an 8-byte atomic
 * tag, written release-last, holding `pos + 1` -- the absolute position
 * just past the record. A reader at absolute position p accepts a record
 * only if its tag equals `p + 1`, which rejects three cases with one
 * comparison:
 *   - never written: a fresh allocation is zeroed, and a published tag is
 *     always nonzero because it is biased past its own position;
 *   - still being written: the tag store has not landed yet, so what sits
 *     there is either zero or a lap-stale smaller value;
 *   - lapped: a newer record's tag is larger.
 *
 * Slot i on lap L therefore holds `L * cap + i + 1`, which is strictly
 * increasing per lap -- the same encoding as ringbuf_zcopy's per-slot
 * stamp, minus the free-versus-reserved distinction. This backend never
 * needs that one: its reserve asks about `head - tail`, never about the
 * previous lap's occupant of a slot.
 *
 * The bias is load-bearing. A bare lap counter would read as 0 for a
 * never-written slot and 0 for every slot on lap 0, so the whole first
 * fill of the ring would be indistinguishable from empty -- a reader would
 * accept uninitialised bytes as a record.
 */

/**
 * struct pf_rec_hdr - The tag every record begins with.
 * @tag: `pos + 1`, as described above.
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
 * No longer required to divide @cap, since @cap counts slots rather than
 * bytes -- which is the whole of what the double mapping used to be
 * for. */
#define PF_REC_BYTES (PF_PAYLOAD_BYTES + sizeof(struct pf_rec_hdr))
_Static_assert(PF_REC_BYTES % PF_REC_ALIGN == 0,
               "the stride keeps every tag 8-byte aligned");

struct pf_ring {
    uint8_t *base; /* cap * PF_REC_BYTES bytes of slot storage */
    size_t cap; /* SLOT COUNT, a power of two */
    size_t mask; /* cap - 1, for mapping a position to a slot */

    /* Lap records the reader has not consumed, rather than refusing new
     * ones. Set for snapshot mode, where overwriting the oldest events is
     * the entire point, and clear for continuous mode, which drops
     * instead. */
    bool overwrite;

    _Atomic uint64_t head; /* next slot to reserve */
    _Atomic uint64_t tail; /* oldest slot the reader still needs */
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
 * Scaled by the stride, to match what pf_ring_capacity_bytes reports: the
 * counters hold slots, so a byte figure has to be recovered from them.
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
    return (head - tail) * PF_REC_BYTES + need;
}

#endif /* PF_RING_MMAP_H */
