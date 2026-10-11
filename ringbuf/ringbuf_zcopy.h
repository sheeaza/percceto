#ifndef RINGBUF_ZCOPY_H
#define RINGBUF_ZCOPY_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Single-producer / single-consumer ring buffer with a split push:
 * reserve a slot, fill it in place, then submit it.
 *
 * Every other variant in this tree copies the payload inside push(), from a
 * buffer the caller already filled. This one hands the caller a pointer to
 * the slot itself and lets it write there directly:
 *
 *	struct ringbuf_rsv a, b, c;
 *	ringbuf_zcopy_reserve(rb, &a);
 *	ringbuf_zcopy_reserve(rb, &b);
 *	ringbuf_zcopy_reserve(rb, &c);
 *
 *	fill(a.data);		-- these three may run CONCURRENTLY, on
 *	fill(b.data);		-- other threads, in a DMA engine, as an
 *	fill(c.data);		-- in-flight read(2), ...
 *
 *	ringbuf_zcopy_submit(rb, &c);	-- in ANY order
 *	ringbuf_zcopy_submit(rb, &a);
 *	ringbuf_zcopy_submit(rb, &b);
 *
 * Two things that buys: the fill writes straight into the slot, with no
 * staging buffer, and several fills can overlap each other instead of being
 * serialized by one thread's memcpy. Messages are delivered in RESERVATION
 * order regardless of submit order -- submitting c first does not deliver it
 * first.
 *
 * The contract, which is SPSC like ringbuf.h and ringbuf_mutex.h:
 *
 *   - Exactly ONE thread may call ringbuf_zcopy_reserve() (or the push()
 *     convenience wrapper). More than one is undefined behavior.
 *   - Exactly ONE thread may call ringbuf_zcopy_pop(). More than one is
 *     undefined behavior.
 *   - ringbuf_zcopy_submit() may be called from ANY thread, which is the
 *     point: it is what the filler threads call when they finish.
 *
 * EVERY SUCCESSFUL RESERVATION MUST BE SUBMITTED, exactly once. There is no
 * cancel. Abandoning a reservation stalls delivery permanently: the consumer
 * waits for that slot forever and the buffer wedges once the producer laps
 * the ring. On an error path, submit a sentinel payload the consumer can
 * recognize -- do not simply drop the handle.
 *
 * BOTH SIDES MUST RETRY. pop() can return false while the buffer is not
 * empty, because a reserved-but-unsubmitted slot blocks delivery
 * head-of-line; reserve() can return false for the mirror-image reason. See
 * the notes on the two functions. A caller that treats false as terminal
 * will stall.
 *
 * Relation to the other variants. This is ringbuf_mutex.h with push() split
 * in two: same data structure, same SPSC contract, same two modes, and
 * overwrite mode is still reconciled with one internal mutex, so
 * reserve()/push() may BLOCK and neither is async-signal-safe. Use ringbuf.h
 * if that matters.
 *
 * The one structural difference follows from out-of-order submit. In
 * ringbuf_mutex.h the write index doubles as the readiness marker: everything
 * below it is stored, everything at or above it is not. That cannot work here
 * -- reservations are handed out in order but completed in any order, so
 * "stored" is no longer a prefix of the ring. Readiness therefore moves to a
 * per-slot flag, one per slot, which is what submit sets and pop tests.
 *
 * Used only through ringbuf_zcopy_push(), there are never any outstanding
 * reservations, and the variant is contract-identical to ringbuf_mutex.h --
 * including overwrite mode's promise that push never fails.
 */

struct ringbuf_zcopy;

/**
 * struct ringbuf_rsv - a claimed slot, awaiting submit
 * @data: slot_size writable bytes; fill these before submitting
 * @ticket: internal identifier of the claimed slot; do not interpret it
 *
 * Caller-allocated, unlike the opaque buffer handle -- stack-allocate one
 * per reservation. Filled in by ringbuf_zcopy_reserve() and consumed by
 * ringbuf_zcopy_submit(). Valid until submitted, after which @data must not
 * be touched again: the slot belongs to the consumer.
 *
 * @data points into the buffer's own storage, so it stays valid across other
 * reservations and pops, and until ringbuf_zcopy_destroy().
 */
struct ringbuf_rsv {
    void *data;
    size_t ticket;
};

/**
 * ringbuf_zcopy_create - allocate a ring buffer
 * @slot_size: size of each slot in bytes; must be non-zero
 * @min_capacity: lower bound on the slot count, rounded up to the next
 *	power of two >= @min_capacity (minimum 2)
 * @overwrite_on_full: if true, a reserve on a full buffer evicts the oldest
 *	unread slot (counted in ringbuf_zcopy_dropped_count()) instead of
 *	failing. If false, a reserve on a full buffer fails.
 *
 * Return: the new buffer, or NULL on allocation failure, mutex-init
 * failure, or invalid arguments.
 */
struct ringbuf_zcopy *ringbuf_zcopy_create(size_t slot_size,
                                           size_t min_capacity,
                                           bool overwrite_on_full);

/**
 * ringbuf_zcopy_destroy - free a ring buffer
 * @rb: buffer from ringbuf_zcopy_create(), or NULL
 *
 * No producer or consumer may be running concurrently, and no reservation
 * may still be outstanding -- @rb owns the memory every handle points into.
 */
void ringbuf_zcopy_destroy(struct ringbuf_zcopy *rb);

/**
 * ringbuf_zcopy_reserve - claim a slot to fill in place
 * @rb: the buffer
 * @rsv: handle to fill in, valid only on a true return
 *
 * Producer side only, and single-caller. On success @rsv->data points at
 * slot_size writable bytes that are yours until you submit; nothing is
 * delivered to the consumer until ringbuf_zcopy_submit() is called on this
 * exact handle, which you MUST do exactly once (see the note at the top of
 * this header -- there is no cancel).
 *
 * Any number of reservations may be outstanding at once, up to the
 * buffer's capacity, and they may be submitted in any order.
 *
 * A false return has two causes, and neither is an error:
 *
 *   - The buffer is full of unread messages. Lossless mode only; in
 *     overwrite mode this evicts instead, and may block briefly on the
 *     consumer rather than failing.
 *   - The slot is still held by an earlier reservation of yours that has
 *     not been submitted yet. This happens in BOTH modes -- an unsubmitted
 *     slot is never evicted, since a filler may be writing to it. It is
 *     about the reservation's age rather than how many are outstanding:
 *     the slot wanted here was reserved exactly capacity tickets ago. Only
 *     your own submit can clear it, so retrying without submitting
 *     something will spin forever.
 *
 * Return: true if a slot was claimed and @rsv filled in; false if the
 * buffer is full (lossless mode) or the slot it needs is reserved but
 * unsubmitted (either mode).
 */
bool ringbuf_zcopy_reserve(struct ringbuf_zcopy *rb, struct ringbuf_rsv *rsv);

/**
 * ringbuf_zcopy_submit - publish a filled reservation to the consumer
 * @rb: the buffer
 * @rsv: handle from a successful ringbuf_zcopy_reserve()
 *
 * Callable from ANY thread -- in particular the one that filled the slot,
 * which is what lets several fills proceed in parallel. Never fails, never
 * blocks, and takes no lock in either mode. Concurrent submits of distinct
 * reservations are safe; they touch disjoint slots.
 *
 * Exactly once per reservation. Submitting a handle twice, or one that
 * reserve() did not fill in, is undefined behavior.
 *
 * After this returns, @rsv->data must not be read or written again.
 *
 * The message becomes visible to the consumer in RESERVATION order, not
 * submit order: an earlier reservation that is still unsubmitted keeps this
 * one waiting, and until it arrives ringbuf_zcopy_pop() reports false even
 * though the buffer is not empty.
 */
void ringbuf_zcopy_submit(struct ringbuf_zcopy *rb,
                          const struct ringbuf_rsv *rsv);

/**
 * ringbuf_zcopy_push - reserve, copy one message in, and submit
 * @rb: the buffer
 * @data: slot_size bytes to copy in
 *
 * Producer side only. Convenience wrapper for callers that already have the
 * message assembled elsewhere and gain nothing from filling in place; it
 * leaves no reservation outstanding on return.
 *
 * Used exclusively, this makes the buffer behave exactly like
 * ringbuf_mutex.h: since there is never an unsubmitted slot, the second
 * cause of a false return from reserve() cannot arise, so in overwrite mode
 * this ALWAYS returns true.
 *
 * Return: true if the message was stored. In lossless mode, false if the
 * buffer is full. In overwrite mode, always true: it evicts the oldest
 * unread slot and counts it in ringbuf_zcopy_dropped_count(), blocking
 * briefly if the consumer is mid-read of that exact slot.
 */
bool ringbuf_zcopy_push(struct ringbuf_zcopy *rb, const void *data);

/**
 * ringbuf_zcopy_pop - copy one message out of the buffer
 * @rb: the buffer
 * @data: destination for slot_size bytes
 *
 * Consumer side only, and single-caller. On a false return the contents of
 * @data are unspecified and must not be used.
 *
 * A false return does NOT imply the buffer is empty, in either mode. The
 * oldest slot may be reserved but not yet submitted, which blocks delivery
 * head-of-line: later messages can already be sitting in the ring, but
 * handing them out early would reorder the stream. This is inherent to the
 * split push, and it is why the consumer must retry rather than treating
 * false as "drained". Use ringbuf_zcopy_is_empty_approx() if you need to
 * distinguish the two, bearing in mind that it is approximate.
 *
 * Return: true if a message was copied out; false if the buffer is empty or
 * the oldest slot is still awaiting its submit.
 */
bool ringbuf_zcopy_pop(struct ringbuf_zcopy *rb, void *data);

/**
 * ringbuf_zcopy_capacity - slot count of the buffer
 * @rb: the buffer
 *
 * Return: the capacity chosen at create time; it never changes.
 */
size_t ringbuf_zcopy_capacity(const struct ringbuf_zcopy *rb);

/**
 * ringbuf_zcopy_slot_size - size of one slot
 * @rb: the buffer
 *
 * Return: the slot size in bytes, as passed to ringbuf_zcopy_create(). This
 * is how many bytes a reservation's @data points at.
 */
size_t ringbuf_zcopy_slot_size(const struct ringbuf_zcopy *rb);

/**
 * ringbuf_zcopy_dropped_count - messages evicted before they were read
 * @rb: the buffer
 *
 * Return: the number of messages reserve() has evicted. Always 0 if the
 * buffer was created with overwrite_on_full = false.
 */
size_t ringbuf_zcopy_dropped_count(const struct ringbuf_zcopy *rb);

/**
 * ringbuf_zcopy_is_empty_approx - diagnostic emptiness check
 * @rb: the buffer
 *
 * Return: true if the buffer looked empty. The result may already be stale
 * by the time it is read, since the other side can concurrently
 * reserve/submit/pop. Counts reservations, so a slot still awaiting its
 * submit makes this report non-empty while ringbuf_zcopy_pop() still
 * returns false.
 */
bool ringbuf_zcopy_is_empty_approx(const struct ringbuf_zcopy *rb);

/**
 * ringbuf_zcopy_is_full_approx - diagnostic fullness check
 * @rb: the buffer
 *
 * Return: true if the buffer looked full. The result may already be stale
 * by the time it is read, since the other side can concurrently
 * reserve/submit/pop. Counts reservations, so outstanding ones count as
 * occupying their slots -- which they do.
 */
bool ringbuf_zcopy_is_full_approx(const struct ringbuf_zcopy *rb);

#ifdef RINGBUF_TEST_HOOKS
/**
 * ringbuf_zcopy_test_seed_counters - reposition the counters, tests only
 * @rb: the buffer, which must be empty
 * @start: value to park both internal counters at
 *
 * Compiled in only under -DRINGBUF_TEST_HOOKS. Leaves the buffer empty.
 * This exists so tests can exercise the point where the counters overflow
 * SIZE_MAX, which would otherwise require 2^64 reservations to reach.
 *
 * Also reinitializes the per-slot stamps, which hold absolute ticket
 * numbers and would otherwise desynchronize from the seeded counters.
 *
 * Preconditions: the buffer must be empty with no reservation outstanding,
 * and no producer or consumer may be running concurrently.
 */
void ringbuf_zcopy_test_seed_counters(struct ringbuf_zcopy *rb, size_t start);
#endif

#endif /* RINGBUF_ZCOPY_H */
