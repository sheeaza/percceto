#ifndef RINGBUF_VSLOT_H
#define RINGBUF_VSLOT_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Single-producer / single-consumer lock-free ring buffer, variable-length
 * messages.
 *
 * Same external SPSC contract as ringbuf.h: exactly one thread may call
 * ringbuf_vslot_push(); exactly one (possibly different) thread may call
 * ringbuf_vslot_pop(). Calling either from more than one thread concurrently
 * is undefined behavior.
 *
 * What differs is the storage model. ringbuf.h and ringbuf_mutex.h hand out
 * fixed-size slots: every message occupies slot_size bytes whatever its
 * real length, and the receiver cannot tell how many of those bytes meant
 * anything. Here each message carries its own length, records are packed
 * densely, and the buffer is sized in BYTES rather than slots -- so memory
 * tracks the sizes actually pushed instead of the worst case.
 *
 * The cost of that density, and the reason this file is a third variant
 * rather than an option on the first:
 *
 *   - Messages longer than max_msg_size are TRUNCATED, not rejected. See
 *     ringbuf_vslot_push().
 *   - There is no fixed message capacity to report, and no meaningful
 *     "is full": whether a push fits depends on its size. Use
 *     ringbuf_vslot_free_bytes_approx().
 *   - LINUX ONLY. The buffer is a mirrored mapping (the same pages mapped
 *     twice, back to back) so that a record straddling the physical end of
 *     the ring stays contiguous in virtual memory. That needs
 *     memfd_create(2), and it means the storage is an mmap region rather
 *     than part of the control block's allocation.
 *
 * Like ringbuf.h and unlike ringbuf_mutex.h, this variant is lock-free:
 * ringbuf_vslot_push() never blocks, so it is usable from a signal handler.
 * The price is the same one ringbuf.h pays -- in overwrite mode both push
 * and pop can fail transiently on contention, so EVERY CALLER MUST RETRY.
 */

struct ringbuf_vslot;

/**
 * ringbuf_vslot_create - allocate a variable-length ring buffer
 * @max_msg_size: PER-MESSAGE ceiling, in bytes; must be non-zero. Sets the
 *	largest message body the buffer keeps intact: a push longer than
 *	this is truncated to it rather than refused, so pick the length
 *	beyond which a message's tail does not matter to you. It also sizes
 *	the receive buffer a consumer needs to be sure of never truncating
 *	(see ringbuf_vslot_pop()), and it forms a floor under the ring --
 *	the capacity is raised if needed so one message this size always
 *	fits.
 * @min_ring_bytes: WHOLE-BUFFER floor, in bytes -- the total storage for
 *	all queued messages, not a per-message figure and not a message
 *	count. Sets how much unread data can be in flight, so it is what
 *	trades memory against how far the consumer may fall behind before
 *	pushes start failing (lossless mode) or evicting (overwrite mode).
 *	Rounded UP to a power of two, to at least one page, and to at least
 *	one max_msg_size message, so a small request can yield a
 *	substantially larger ring -- ringbuf_vslot_capacity_bytes() reports
 *	what you actually got.
 * @overwrite_on_full: if true, ringbuf_vslot_push() does not fail for want of
 *	room: it evicts as many of the oldest unread messages as it needs
 *	(see ringbuf_vslot_push()). If false, a push that does not fit fails.
 *
 * The two sizes are independent axes: @max_msg_size bounds one message,
 * @min_ring_bytes bounds all of them together. Their ratio is roughly how
 * many messages the buffer holds -- only roughly, because capacity is a
 * byte count and each message costs 4 + its own length rounded up to a
 * multiple of 4, so the real figure depends on the sizes actually pushed.
 * A ring of 64 KiB with max_msg_size 1 KiB holds 64 worst-case messages,
 * or several thousand 16-byte ones.
 *
 * Allocates two mappings of the ring (see the file comment), which is
 * 2 * ringbuf_vslot_capacity_bytes() of address space but only one copy of
 * physical memory.
 *
 * Return: the new buffer, or NULL on allocation failure, mapping failure,
 * or invalid arguments.
 */
struct ringbuf_vslot *ringbuf_vslot_create(size_t max_msg_size,
                                           size_t min_ring_bytes,
                                           bool overwrite_on_full);

/**
 * ringbuf_vslot_destroy - free a ring buffer
 * @rb: buffer from ringbuf_vslot_create(), or NULL
 */
void ringbuf_vslot_destroy(struct ringbuf_vslot *rb);

/**
 * ringbuf_vslot_push - copy one message into the buffer
 * @rb: the buffer
 * @data: @size bytes to copy in; may be NULL only if @size is 0
 * @size: length of the message in bytes. 0 is legal and round-trips as a
 *	zero-length message. Anything above the max_msg_size given at create
 *	time is TRUNCATED to max_msg_size -- the push still succeeds, and the
 *	event is counted in ringbuf_vslot_truncated_count().
 *
 * Producer side only. Never blocks.
 *
 * Return: true if the message was stored. If the buffer was created with
 * overwrite_on_full = false, false without blocking when the message does
 * not fit in the free space. If created with overwrite_on_full = true, it
 * instead evicts as many of the oldest unread messages as it needs to make
 * room, counting each in ringbuf_vslot_dropped_count(), and returns true --
 * except for a rare, transient false return if the consumer happens to be
 * mid-read of the oldest message, which the caller's normal retry handles.
 */
bool ringbuf_vslot_push(struct ringbuf_vslot *rb, const void *data,
                        size_t size);

/**
 * ringbuf_vslot_pop - copy one message out of the buffer
 * @rb: the buffer
 * @data: destination; may be NULL only if @cap is 0
 * @cap: bytes available at @data. A message longer than this is truncated:
 *	exactly @cap bytes are written and the rest of it is discarded along
 *	with the message.
 * @len: out-parameter for the message's STORED length, which is set on
 *	every true return and may exceed @cap. Testing *@len > @cap is how a
 *	caller detects that it lost the tail; *@len <= @cap means the whole
 *	message was copied. Must not be NULL.
 *
 * Consumer side only. Never blocks. On a false return, the contents of
 * @data and *@len are unspecified and must not be used.
 *
 * Pass cap >= ringbuf_vslot_max_msg_size() to make truncation here impossible.
 *
 * Return: true if a message was copied out. false if the buffer is empty,
 * or -- in overwrite_on_full mode only -- if the producer is evicting this
 * exact message right now; the caller's normal retry handles the latter.
 */
bool ringbuf_vslot_pop(struct ringbuf_vslot *rb, void *data, size_t cap,
                       size_t *len);

/**
 * ringbuf_vslot_capacity_bytes - whole-buffer storage, in bytes
 * @rb: the buffer
 *
 * What @min_ring_bytes was rounded up to at create time; it never changes.
 * A byte count covering all queued messages, NOT a message count: each
 * message costs 4 + its length rounded up to a multiple of 4, so how many
 * fit depends on the sizes pushed.
 *
 * Return: the capacity in bytes.
 */
size_t ringbuf_vslot_capacity_bytes(const struct ringbuf_vslot *rb);

/**
 * ringbuf_vslot_max_msg_size - per-message ceiling, in bytes
 * @rb: the buffer
 *
 * The largest message body kept intact; longer pushes are truncated to it.
 * A receive buffer of this size is always enough for ringbuf_vslot_pop()
 * to return a message whole.
 *
 * Return: max_msg_size as passed to ringbuf_vslot_create().
 */
size_t ringbuf_vslot_max_msg_size(const struct ringbuf_vslot *rb);

/**
 * ringbuf_vslot_dropped_count - messages evicted before they were read
 * @rb: the buffer
 *
 * Return: the number of messages ringbuf_vslot_push() has evicted. Always 0
 * if the buffer was created with overwrite_on_full = false.
 */
size_t ringbuf_vslot_dropped_count(const struct ringbuf_vslot *rb);

/**
 * ringbuf_vslot_truncated_count - messages stored with their tail cut off
 * @rb: the buffer
 *
 * Counts producer-side truncation only -- messages pushed with size >
 * max_msg_size. Truncation inside ringbuf_vslot_pop() from too small a @cap is
 * the caller's own doing and is reported through its @len instead.
 *
 * Return: the number of messages ringbuf_vslot_push() has truncated.
 */
size_t ringbuf_vslot_truncated_count(const struct ringbuf_vslot *rb);

/**
 * ringbuf_vslot_is_empty_approx - diagnostic emptiness check
 * @rb: the buffer
 *
 * Return: true if the buffer looked empty. The result may already be stale
 * by the time it is read, since the other side can concurrently push/pop.
 */
bool ringbuf_vslot_is_empty_approx(const struct ringbuf_vslot *rb);

/**
 * ringbuf_vslot_free_bytes_approx - diagnostic free-space check
 * @rb: the buffer
 *
 * This variant has no "is full": whether a push fits depends on its size.
 * A message of @size bytes needs align_up(4 + @size, 4) of free space.
 *
 * Return: the free space in bytes. The result may already be stale by the
 * time it is read, since the other side can concurrently push/pop.
 */
size_t ringbuf_vslot_free_bytes_approx(const struct ringbuf_vslot *rb);

#ifdef RINGBUF_TEST_HOOKS
/**
 * ringbuf_vslot_test_seed_counters - reposition both counters, tests only
 * @rb: the buffer, which must be empty
 * @start: value to park both internal counters at; rounded down to a
 *	multiple of 4, since records are 4-byte aligned
 *
 * Compiled in only under -DRINGBUF_TEST_HOOKS. Leaves the buffer empty.
 * This exists so tests can exercise the point where the counters overflow
 * SIZE_MAX, which would otherwise require SIZE_MAX bytes of pushes to
 * reach.
 *
 * Preconditions: the buffer must be empty, and no producer or consumer may
 * be running concurrently.
 */
void ringbuf_vslot_test_seed_counters(struct ringbuf_vslot *rb, size_t start);
#endif

#endif /* RINGBUF_VSLOT_H */
