#ifndef PF_RING_H
#define PF_RING_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Lockless fixed-stride record ring, in a fixed-size mapping made at init
 * and never resized.
 *
 * Every record is exactly %PF_REC_BYTES, which is what makes the rest of
 * this file small: a record boundary is any multiple of the stride, so
 * @tail can be moved by arithmetic instead of by walking tags, there is no
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
 * Positions (@head, @tail) are absolute monotonic byte counters, never
 * wrapped; a buffer offset is `pos & mask`. Keeping them absolute is what
 * makes the tag check below work, and it sidesteps the
 * full-versus-empty ambiguity of wrapped indices.
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

/* Every record is a multiple of 8 bytes, so each tag stays naturally
 * aligned and its load/store is a plain instruction plus a fence. */
#define PF_REC_ALIGN 8u

/**
 * struct pf_rec_hdr - The tag every record begins with.
 * @tag: `pos + PF_REC_BYTES`, as described above.
 *
 * Owned by pf_ring, not by the code storing records in it: pf_ring_submit
 * writes @tag release-last and the reader's check is a load of it, so
 * a producer reserves space for this header plus its payload, fills only
 * the payload, and leaves @tag alone. That split is what makes publication
 * atomic per record.
 *
 * Callers embed this as their record's first member -- see struct pf_rec in
 * pftrace.c -- so the payload offsets either side of the ring agree by
 * construction instead of by a matching pair of constants.
 */
struct pf_rec_hdr {
    _Atomic uint64_t tag;
};

/* The stride, and so also the largest (and only) record size. Every
 * position the ring hands out is a multiple of this.
 *
 * pftrace.c pins its struct pf_rec to this with a _Static_assert; the two
 * must agree, and this is the one that pf_ring's arithmetic uses. */
#define PF_REC_BYTES 40u
_Static_assert(PF_REC_BYTES % PF_REC_ALIGN == 0,
               "the stride keeps every tag 8-byte aligned");

#define PF_RING_MIN_BYTES (64u * 1024u)

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

/** struct pf_ring_reader - A walk in progress over published records. */
struct pf_ring_reader {
    uint64_t pos;
    uint64_t end;
};

/**
 * pf_ring_init() - Map the ring.
 * @r: Ring to initialize.
 * @want_bytes: Requested capacity; rounded up to a power of two, with a
 *      floor of %PF_RING_MIN_BYTES. Since the result is a power of two no
 *      smaller than 64 KiB it is necessarily a page multiple too.
 * @overwrite: Overwrite unconsumed records rather than dropping new ones.
 *
 * Return: 0 on success, -1 with errno set on failure.
 */
int pf_ring_init(struct pf_ring *r, size_t want_bytes, bool overwrite);

/**
 * pf_ring_destroy() - Unmap the ring.
 * @r: Ring to destroy. Safe on an already-destroyed or zeroed ring.
 */
void pf_ring_destroy(struct pf_ring *r);

/**
 * struct pf_rec_res - A reservation held between reserve and submit.
 * @rec: Where to write the record, or %NULL if it was dropped.
 * @tag: The tag pf_ring_submit() will store. Opaque to the caller.
 *
 * @tag cannot simply live in @rec until submit: the tag's value is exactly
 * what the reader accepts, so a half-filled record would read as
 * published. Nor can submit recompute it from @rec alone -- a pointer into
 * the mapping yields `pos & mask`, not the absolute position -- and
 * recovering it from @head would get it wrong for a producer delayed a
 * full lap, stamping a live record with a tag that validates. Keeping it
 * here, outside the ring, is what makes a lapped producer's submit land as
 * a stale tag the reader rejects.
 *
 * Two words, so it returns in registers rather than through memory.
 */
struct pf_rec_res {
    struct pf_rec_hdr *rec;
    uint64_t tag;
};

/**
 * pf_ring_reserve() - Reserve space for one record.
 * @r: Ring.
 *
 * Reserves the next %PF_REC_BYTES, owned exclusively by the caller, who
 * may then fill them concurrently with other producers filling theirs.
 * Write the payload after the returned header and leave the header itself
 * alone; it is pf_ring_submit()'s to store.
 *
 * Without @overwrite this reserves with a compare-exchange rather than a
 * fetch-add, so that a refusal for lack of space leaves @head untouched. A
 * fetch-add would advance @head first and then have nowhere to put a valid
 * tag, leaving a permanent hole that the reader could never walk past.
 *
 * Return: The reservation, whose @rec is %NULL if the record was dropped
 * (only without @overwrite, reader too far behind). A dropped reservation
 * must not be submitted.
 */
struct pf_rec_res pf_ring_reserve(struct pf_ring *r);

/**
 * pf_ring_submit() - Make a filled record visible to the reader.
 * @r: Ring.
 * @res: Reservation from pf_ring_reserve(), whose @rec is not %NULL.
 *
 * Release-stores the tag, so a reader that acquire-loads it sees the
 * fully written payload.
 */
void pf_ring_submit(struct pf_ring *r, struct pf_rec_res res);

/**
 * pf_ring_read_begin() - Start a walk at the oldest unconsumed record.
 * @r: Ring.
 * @rd: Walk state to initialize.
 */
void pf_ring_read_begin(struct pf_ring *r, struct pf_ring_reader *rd);

/**
 * pf_ring_next() - Advance to the next published record.
 * @r: Ring.
 * @rd: Walk state.
 *
 * Stops at the first record that is not yet published, which costs at
 * most one in-flight memcpy of latency and resolves on the next walk.
 *
 * Return: Pointer to the record, contiguous for %PF_REC_BYTES thanks to
 * the mirror, or %NULL at the end of the published run.
 */
const uint8_t *pf_ring_next(struct pf_ring *r, struct pf_ring_reader *rd);

/**
 * pf_ring_still_valid() - Check a record was not overwritten while read.
 * @r: Ring.
 * @pos: Absolute position of the record being checked.
 *
 * Only an @overwrite (snapshot) ring can overwrite a record out from under
 * the reader, so this always succeeds otherwise. Call it after
 * consuming a record's bytes and discard the result if it fails; this is
 * the second half of a seqlock, with pf_ring_next()'s tag check as the
 * first. Still required despite the fixed stride: the tag check alone
 * leaves a window in which a producer laps the slot after the reader has
 * accepted it but before it has finished copying the body.
 *
 * Return: %true if the record's bytes are still intact.
 */
bool pf_ring_still_valid(const struct pf_ring *r, uint64_t pos);

/**
 * pf_ring_read_commit() - Release every record walked so far.
 * @r: Ring.
 * @rd: Walk state.
 *
 * Advances @tail, freeing the space for producers. Without @overwrite this
 * is what lets them stop dropping.
 */
void pf_ring_read_commit(struct pf_ring *r, struct pf_ring_reader *rd);

/**
 * pf_ring_trim() - Drop the oldest records to keep a safety margin.
 * @r: Ring.
 * @retain_bytes: Most recent bytes worth keeping. Rounded down to a
 *      whole number of records, so @tail lands on a boundary.
 *
 * Snapshot mode never consumes records, so without this @tail would stay
 * at 0 while producers lap the buffer repeatedly, and the reader would
 * start from a position long overwritten. Moving @tail forward keeps a
 * margin between the newest producer and the oldest retained record, so a
 * dump is never racing a wrap.
 *
 * Cheap: with a fixed stride every multiple of %PF_REC_BYTES is a record
 * boundary, so this is arithmetic plus one compare-exchange. It reads no
 * tags and cannot fail, which is why it has no return value -- a
 * variable-length ring would have to walk the records to find a boundary,
 * and could lose the race to a producer recycling the one it started from.
 */
void pf_ring_trim(struct pf_ring *r, size_t retain_bytes);

/**
 * pf_ring_resync() - Abandon buffered records after losing the window.
 * @r: Ring.
 *
 * Moves @tail to @head, which is always a record boundary, discarding what
 * had not been read and bumping the resync counter.
 *
 * Reached when a reader finds that a producer lapped the record it was
 * copying -- pf_ring_still_valid() failing mid-walk. With @overwrite that
 * is a normal outcome under load, not a fault: there is no backpressure to
 * prevent it. Losing the older part of the window is the lesser cost, and
 * the recent records a snapshot is actually for survive.
 *
 * Note pf_ring_trim() does not need this. With a fixed stride it derives a
 * boundary arithmetically rather than by walking records, so there is no
 * boundary for a producer to recycle out from under it.
 */
void pf_ring_resync(struct pf_ring *r);

#endif /* PF_RING_H */
