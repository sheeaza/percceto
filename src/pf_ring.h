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
 * Publication is per record, because producers claim space in order but
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
#define PF_REC_TAG_BYTES 8u

/* 8 tag + 8 ts + 8 track_uuid + 4 type/nlen/clen/flags + 8 value
 * + 255 name + 255 category, rounded up to PF_REC_ALIGN. */
#define PF_REC_MAX 552u

#define PF_RING_MIN_BYTES (64u * 1024u)

struct pf_ring {
    uint8_t *base; /* first of two mappings of the same pages */
    size_t cap; /* power of two, hence also a page multiple */
    size_t mask; /* cap - 1 */
    unsigned log2cap;

    /* Refuse to overwrite records the reader has not consumed, dropping
     * instead. Set for continuous mode, clear for snapshot mode, where
     * overwriting the oldest events is the entire point. */
    bool bounded;
    bool single_producer;

    _Atomic uint64_t head; /* next byte to claim */
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
 * @bounded: Drop rather than overwrite unconsumed records.
 * @single_producer: Only one thread will ever claim, so claiming can skip
 *      the atomic read-modify-write.
 *
 * Return: 0 on success, -1 with errno set on failure.
 */
int pf_ring_init(struct pf_ring *r, size_t want_bytes, bool bounded,
                 bool single_producer);

/**
 * pf_ring_destroy() - Unmap the ring.
 * @r: Ring to destroy. Safe on an already-destroyed or zeroed ring.
 */
void pf_ring_destroy(struct pf_ring *r);

/**
 * pf_ring_claim() - Reserve space for one record.
 * @r: Ring.
 * @need: Record size including the tag; must be 8-byte aligned and at
 *      most %PF_REC_MAX.
 * @off_out: Receives the record's absolute position, to hand to
 *      pf_ring_publish().
 *
 * Reserves a byte range owned exclusively by the caller, who may then
 * fill it concurrently with other producers filling theirs. Writes the
 * payload at the returned pointer plus %PF_REC_TAG_BYTES, leaving the tag
 * for pf_ring_publish().
 *
 * In bounded mode this claims with a compare-exchange rather than a
 * fetch-add, so that a claim refused for lack of space leaves @head
 * untouched. A fetch-add would advance @head first and then have nowhere
 * to put a valid tag, leaving a permanent hole that the reader could
 * never walk past.
 *
 * Return: Pointer to the record start, or %NULL if the record was
 * dropped (bounded mode, reader too far behind).
 */
uint8_t *pf_ring_claim(struct pf_ring *r, uint32_t need, uint64_t *off_out);

/**
 * pf_ring_publish() - Make a filled record visible to the reader.
 * @r: Ring.
 * @off: Absolute position from pf_ring_claim().
 * @need: Same size passed to pf_ring_claim().
 *
 * Release-stores the tag, so a reader that acquire-loads it sees the
 * fully written payload.
 */
void pf_ring_publish(struct pf_ring *r, uint64_t off, uint32_t need);

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
 * Only unbounded (snapshot) mode can overwrite a record out from under
 * the reader, so this always succeeds in bounded mode. Call it after
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
 * Advances @tail, freeing the space for producers. In bounded mode this
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
 * is deriving a boundary from it. In unbounded mode that is a normal
 * outcome under load, not a fault: there is no backpressure to prevent it,
 * and the alternative -- leaving @tail mid-record -- is unrecoverable,
 * since lengths are only discoverable forwards. Losing the older part of
 * the window is the lesser cost, and the recent records a snapshot is
 * actually for survive.
 */
void pf_ring_resync(struct pf_ring *r);

#endif /* PF_RING_H */
