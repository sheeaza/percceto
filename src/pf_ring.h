#ifndef PF_RING_H
#define PF_RING_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Lockless variable-length record ring, in a fixed-size mapping made at
 * init and never resized.
 *
 * Wrap handling is delegated to the MMU rather than to branches: the same
 * physical pages are mapped twice, back to back, so a record that runs off
 * the end of the buffer continues into the mirror and the kernel folds
 * those bytes back to the front. Producers and the reader therefore treat
 * every record as contiguous -- no split memcpy, no padding records, and
 * no wasted tail bytes.
 *
 * Positions (@head, @tail) are absolute monotonic byte counters, never
 * wrapped; a buffer offset is `pos & mask`. Keeping them absolute is what
 * makes the lap check below work, and it sidesteps the
 * full-versus-empty ambiguity of wrapped indices.
 *
 * Publication is per record, because producers reserve space in order but
 * finish copying out of order. Each record starts with an 8-byte atomic
 * tag, `(lap << 32) | len`, written release-last. A reader at absolute
 * position p accepts a record only if its lap equals `p >> log2cap`,
 * which rejects in three cases with one comparison:
 *   - never written: fresh mapping pages read as zero, so lap 0 and
 *     len 0, and a real record always has len > 0;
 *   - still being written: the tag store has not landed yet;
 *   - lapped: a newer record from a later lap already sits there.
 */

/* Every record is a multiple of 8 bytes, so each tag stays naturally
 * aligned and its load/store is a plain instruction plus a fence. */
#define PF_REC_ALIGN 8u

/**
 * struct pf_rec_hdr - The tag every record begins with.
 * @tag: `(lap << 32) | len`, as described above.
 *
 * Owned by pf_ring, not by the code storing records in it: pf_ring_submit
 * writes @tag release-last and the reader's lap check is a load of it, so
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

/* sizeof(struct pf_rec_hdr) + 8 ts + 8 track_uuid
 * + 4 type/nlen/clen/flags + 8 value + 255 name + 255 category,
 * rounded up to PF_REC_ALIGN. */
#define PF_REC_MAX 552u

#define PF_RING_MIN_BYTES (64u * 1024u)

struct pf_ring {
    uint8_t *base; /* first of two mappings of the same pages */
    size_t cap; /* power of two, hence also a page multiple */
    size_t mask; /* cap - 1 */
    unsigned log2cap;

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
 * @tag cannot simply live in @rec until submit: a tag carrying a nonzero
 * length is exactly what the reader accepts, so a half-filled record would
 * read as published. Nor can submit recompute it from @rec alone -- a
 * pointer into the mapping yields `pos & mask`, neither the lap nor the
 * length -- and recovering the lap from @head would get it wrong for a
 * producer delayed a full lap, stamping a live record's lap with this
 * record's length. Keeping it here, outside the ring, is what makes a
 * lapped producer's submit land as a stale tag the reader rejects.
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
 * @need: Record size including the header; must be 8-byte aligned and at
 *      most %PF_REC_MAX.
 *
 * Reserves a byte range owned exclusively by the caller, who may then fill
 * it concurrently with other producers filling theirs. Write the payload
 * after the returned header and leave the header itself alone; it is
 * pf_ring_submit()'s to store.
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
struct pf_rec_res pf_ring_reserve(struct pf_ring *r, uint32_t need);

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
 * @len_out: Receives the record's total size including its tag.
 *
 * Stops at the first record that is not yet published, which costs at
 * most one in-flight memcpy of latency and resolves on the next walk.
 *
 * Return: Pointer to the record, contiguous for @len_out bytes thanks to
 * the mirror, or %NULL at the end of the published run.
 */
const uint8_t *pf_ring_next(struct pf_ring *r, struct pf_ring_reader *rd,
                            uint32_t *len_out);

/**
 * pf_ring_still_valid() - Check a record was not overwritten while read.
 * @r: Ring.
 * @pos: Absolute position of the record being checked.
 *
 * Only an @overwrite (snapshot) ring can overwrite a record out from under
 * the reader, so this always succeeds otherwise. Call it after
 * consuming a record's bytes and discard the result if it fails; this is
 * the second half of a seqlock, with pf_ring_next()'s lap check as the
 * first.
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
 * @retain_bytes: Most recent bytes worth keeping.
 *
 * Snapshot mode never consumes records, so without this @tail would stay
 * at 0 while producers lap the buffer repeatedly, and the reader's
 * starting point would be long overwritten -- a position it could not
 * recover, since record boundaries are not discoverable by scanning
 * backwards. Walking @tail forward keeps it on a real boundary and keeps
 * a margin between the newest producer and the oldest retained record,
 * so a dump is never racing a wrap.
 *
 * Cheap: reads tags and adds lengths, copying nothing.
 *
 * Return: %true if @tail is on a valid boundary afterwards, %false if the
 * boundary had already been overwritten and the ring was resynced.
 */
bool pf_ring_trim(struct pf_ring *r, size_t retain_bytes);

/**
 * pf_ring_resync() - Abandon buffered records after losing the boundary.
 * @r: Ring.
 *
 * Moves @tail to @head, which is always a record boundary, discarding what
 * had not been read and bumping the resync counter.
 *
 * Reached when a producer recycles the oldest record while pf_ring_trim()
 * is deriving a boundary from it. With @overwrite that is a normal
 * outcome under load, not a fault: there is no backpressure to prevent it,
 * and the alternative -- leaving @tail mid-record -- is unrecoverable,
 * since lengths are only discoverable forwards. Losing the older part of
 * the window is the lesser cost, and the recent records a snapshot is
 * actually for survive.
 */
void pf_ring_resync(struct pf_ring *r);

#endif /* PF_RING_H */
