#ifndef RINGBUF_MPSC_H
#define RINGBUF_MPSC_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Multi-producer / single-consumer lock-free ring buffer.
 *
 * Fixed-size slots, power-of-two capacity, same two modes as ringbuf.h.
 * The difference is in the contract, not the storage model:
 *
 *   - ANY number of threads may call ringbuf_mpsc_push() concurrently.
 *   - Exactly ONE thread may call ringbuf_mpsc_pop(). More than one is
 *     undefined behavior.
 *
 * That is the only axis on which this differs from ringbuf.h. Where
 * ringbuf_mutex.h changes how overwrite mode is synchronized while keeping
 * the SPSC contract, this changes the contract itself: the write index is
 * multi-writer here, so producers claim a slot with a ticket (a CAS on the
 * write index) instead of the single-writer load/store pair ringbuf.c uses.
 *
 * The consumer restriction stays, though for a different reason than in
 * ringbuf.h: ringbuf_mpsc_pop() advances the read index with a plain
 * single-writer store, so two concurrent poppers would deliver the same
 * message twice. Making this MPMC is not a matter of relaxing a comment.
 *
 * Lock-free and async-signal-safe in both modes: no push path ever blocks
 * or takes a mutex, unlike ringbuf_mutex.h.
 *
 * BOTH SIDES MUST RETRY. Push and pop can each return false transiently,
 * while the buffer is neither full nor empty -- see the notes on the two
 * functions below. A caller that treats false as terminal will stall. The
 * tests use bounded spin loops with sched_yield().
 */

struct ringbuf_mpsc;

/**
 * ringbuf_mpsc_create - allocate a ring buffer
 * @slot_size: size of each slot in bytes; must be non-zero
 * @min_capacity: lower bound on the slot count, rounded up to the next
 *	power of two >= @min_capacity (minimum 2)
 * @overwrite_on_full: if true, a push onto a full buffer evicts the oldest
 *	unread slot (counted in ringbuf_mpsc_dropped_count()) instead of
 *	failing. If false, a push onto a full buffer fails.
 *
 * Return: the new buffer, or NULL on allocation failure or invalid
 * arguments.
 */
struct ringbuf_mpsc *ringbuf_mpsc_create(size_t slot_size, size_t min_capacity,
                                         bool overwrite_on_full);

/**
 * ringbuf_mpsc_destroy - free a ring buffer
 * @rb: buffer from ringbuf_mpsc_create(), or NULL
 *
 * No producer or consumer may be running concurrently.
 */
void ringbuf_mpsc_destroy(struct ringbuf_mpsc *rb);

/**
 * ringbuf_mpsc_push - copy one message into the buffer
 * @rb: the buffer
 * @data: slot_size bytes to copy in
 *
 * Callable from any number of threads concurrently. Never blocks.
 *
 * Messages are ordered by the ticket each producer wins, not by when it
 * called -- two producers racing here may be delivered in either order.
 * Each producer's own messages do stay in the order it pushed them.
 *
 * Return: true if the message was stored. false if the buffer is full
 * (lossless mode only), or transiently: in overwrite mode when the consumer
 * holds the slot being evicted. A false return never consumes a ticket, so
 * retrying is always safe.
 */
bool ringbuf_mpsc_push(struct ringbuf_mpsc *rb, const void *data);

/**
 * ringbuf_mpsc_pop - copy one message out of the buffer
 * @rb: the buffer
 * @data: destination for slot_size bytes
 *
 * Consumer side only, and single-caller -- see the note at the top of this
 * header. Never blocks. On a false return the contents of @data are
 * unspecified and must not be used.
 *
 * Unlike ringbuf_pop(), a false return here does NOT imply the buffer is
 * empty, in either mode. A producer that has claimed the oldest ticket but
 * has not yet published its message blocks delivery head-of-line: later
 * messages may already be sitting in the ring, but handing them out early
 * would reorder the stream. This is inherent to multiple producers, and it
 * is why the consumer must also retry rather than treating false as
 * "drained". Use ringbuf_mpsc_is_empty_approx() if you need to distinguish
 * the two, bearing in mind that it is approximate.
 *
 * Return: true if a message was copied out; false if the buffer is empty,
 * if a producer is mid-publish on the oldest message, or -- in overwrite
 * mode -- on transient contention with an evicting producer.
 */
bool ringbuf_mpsc_pop(struct ringbuf_mpsc *rb, void *data);

/**
 * ringbuf_mpsc_capacity - slot count of the buffer
 * @rb: the buffer
 *
 * Return: the capacity chosen at create time; it never changes.
 */
size_t ringbuf_mpsc_capacity(const struct ringbuf_mpsc *rb);

/**
 * ringbuf_mpsc_slot_size - size of one slot
 * @rb: the buffer
 *
 * Return: the slot size in bytes, as passed to ringbuf_mpsc_create().
 */
size_t ringbuf_mpsc_slot_size(const struct ringbuf_mpsc *rb);

/**
 * ringbuf_mpsc_dropped_count - messages evicted before they were read
 * @rb: the buffer
 *
 * Return: the number of messages ringbuf_mpsc_push() has evicted. Always 0
 * if the buffer was created with overwrite_on_full = false.
 */
size_t ringbuf_mpsc_dropped_count(const struct ringbuf_mpsc *rb);

/**
 * ringbuf_mpsc_is_empty_approx - diagnostic emptiness check
 * @rb: the buffer
 *
 * Return: true if the buffer looked empty. The result may already be stale
 * by the time it is read, since producers and the consumer can run
 * concurrently. Counts tickets claimed, so a message still mid-publish
 * makes this report non-empty while ringbuf_mpsc_pop() still returns false.
 */
bool ringbuf_mpsc_is_empty_approx(const struct ringbuf_mpsc *rb);

/**
 * ringbuf_mpsc_is_full_approx - diagnostic fullness check
 * @rb: the buffer
 *
 * Return: true if the buffer looked full. The result may already be stale
 * by the time it is read, since producers and the consumer can run
 * concurrently.
 */
bool ringbuf_mpsc_is_full_approx(const struct ringbuf_mpsc *rb);

#ifdef RINGBUF_TEST_HOOKS
/**
 * ringbuf_mpsc_test_seed_counters - reposition the counters, for tests only
 * @rb: the buffer, which must be empty
 * @start: value to park both internal counters at
 *
 * Compiled in only under -DRINGBUF_TEST_HOOKS. Leaves the buffer empty.
 * This exists so tests can exercise the point where the counters overflow
 * SIZE_MAX, which would otherwise require 2^64 pushes to reach.
 *
 * Also reinitializes the per-slot state this variant keeps alongside the
 * counters, which is why it does more work than its ringbuf.h counterpart.
 *
 * Preconditions: the buffer must be empty, and no producer or consumer may
 * be running concurrently.
 */
void ringbuf_mpsc_test_seed_counters(struct ringbuf_mpsc *rb, size_t start);
#endif

#endif /* RINGBUF_MPSC_H */
