#ifndef RINGBUF_H
#define RINGBUF_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Single-producer / single-consumer lock-free ring buffer.
 *
 * Fixed-size slots, power-of-two capacity. Exactly one thread may call
 * ringbuf_push(); exactly one (possibly different) thread may call
 * ringbuf_pop(). Calling either from more than one thread concurrently
 * is undefined behavior.
 *
 * This holds in both modes. Internally, overwrite_on_full makes the read
 * index multi-writer -- the producer advances it when it evicts -- but
 * that does not widen the contract: ringbuf_pop keeps consumer-private
 * non-atomic state, so a second concurrent popper races regardless of
 * how the index itself is synchronized.
 */

struct ringbuf;

/**
 * ringbuf_create - allocate a ring buffer
 * @slot_size: size of each slot in bytes; must be non-zero
 * @min_capacity: lower bound on the slot count, rounded up to the next
 *	power of two >= @min_capacity (minimum 2)
 * @overwrite_on_full: if true, ringbuf_push() never fails due to a full
 *	buffer: it instead evicts the oldest unread slot (see
 *	ringbuf_push()). If false, ringbuf_push() fails on full.
 *
 * Return: the new buffer, or NULL on allocation failure or invalid
 * arguments.
 */
struct ringbuf *ringbuf_create(size_t slot_size, size_t min_capacity,
                               bool overwrite_on_full);

/**
 * ringbuf_destroy - free a ring buffer
 * @rb: buffer from ringbuf_create(), or NULL
 */
void ringbuf_destroy(struct ringbuf *rb);

/**
 * ringbuf_push - copy one message into the buffer
 * @rb: the buffer
 * @data: slot_size bytes to copy in
 *
 * Producer side only.
 *
 * Return: true if the message was stored. If the buffer was created with
 * overwrite_on_full = false, false without blocking if the buffer is
 * full. If created with overwrite_on_full = true, on full it evicts the
 * oldest unread slot (counted in ringbuf_dropped_count()) and always
 * returns true, except for a rare, transient false return if the consumer
 * happens to be mid-read of that exact slot -- the caller's normal retry
 * handles it.
 */
bool ringbuf_push(struct ringbuf *rb, const void *data);

/**
 * ringbuf_pop - copy one message out of the buffer
 * @rb: the buffer
 * @data: destination for slot_size bytes
 *
 * Consumer side only. Never blocks. On a false return the contents of
 * @data are unspecified and must not be used.
 *
 * Return: true if a message was copied out. false if the buffer is empty,
 * or -- in overwrite_on_full mode only -- if the producer is evicting this
 * exact slot right now, or evicted this message while we were reading it;
 * the caller's normal retry handles those two.
 */
bool ringbuf_pop(struct ringbuf *rb, void *data);

/**
 * ringbuf_capacity - slot count of the buffer
 * @rb: the buffer
 *
 * Return: the capacity chosen at create time; it never changes.
 */
size_t ringbuf_capacity(const struct ringbuf *rb);

/**
 * ringbuf_slot_size - size of one slot
 * @rb: the buffer
 *
 * Return: the slot size in bytes, as passed to ringbuf_create().
 */
size_t ringbuf_slot_size(const struct ringbuf *rb);

/**
 * ringbuf_dropped_count - messages evicted before they were read
 * @rb: the buffer
 *
 * Return: the number of messages ringbuf_push() has evicted. Always 0 if
 * the buffer was created with overwrite_on_full = false.
 */
size_t ringbuf_dropped_count(const struct ringbuf *rb);

/**
 * ringbuf_is_empty_approx - diagnostic emptiness check
 * @rb: the buffer
 *
 * Return: true if the buffer looked empty. The result may already be stale
 * by the time it is read, since the other side can concurrently push/pop.
 */
bool ringbuf_is_empty_approx(const struct ringbuf *rb);

/**
 * ringbuf_is_full_approx - diagnostic fullness check
 * @rb: the buffer
 *
 * Return: true if the buffer looked full. The result may already be stale
 * by the time it is read, since the other side can concurrently push/pop.
 */
bool ringbuf_is_full_approx(const struct ringbuf *rb);

#ifdef RINGBUF_TEST_HOOKS
/**
 * ringbuf_test_seed_counters - reposition both counters, for tests only
 * @rb: the buffer, which must be empty
 * @start: value to park both internal counters at
 *
 * Compiled in only under -DRINGBUF_TEST_HOOKS. Leaves the buffer empty.
 * This exists so tests can exercise the point where the counters overflow
 * SIZE_MAX, which would otherwise require 2^64 pushes to reach.
 *
 * Preconditions: the buffer must be empty, and no producer or consumer may
 * be running concurrently.
 */
void ringbuf_test_seed_counters(struct ringbuf *rb, size_t start);
#endif

#endif /* RINGBUF_H */
