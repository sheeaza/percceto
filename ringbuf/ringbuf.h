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
 */

struct ringbuf;

/* Creates a ring buffer holding slots of slot_size bytes, with capacity
 * rounded up to the next power of two >= min_capacity (minimum 2).
 *
 * If overwrite_on_full is true, ringbuf_push never fails due to a full
 * buffer: it instead evicts the oldest unread slot (see ringbuf_push).
 * If false, ringbuf_push fails on full as before.
 *
 * Returns NULL on allocation failure or invalid arguments. */
struct ringbuf *ringbuf_create(size_t slot_size, size_t min_capacity,
                               bool overwrite_on_full);

/* Frees a ring buffer created by ringbuf_create. */
void ringbuf_destroy(struct ringbuf *rb);

/* Producer side only. Copies slot_size bytes from data into the buffer.
 *
 * If the buffer was created with overwrite_on_full = false: returns false
 * without blocking if the buffer is full.
 *
 * If created with overwrite_on_full = true: on full, evicts the oldest
 * unread slot (counted in ringbuf_dropped_count) and always succeeds,
 * except for a rare, transient false return if the consumer happens to
 * be mid-read of that exact slot — the caller's normal retry handles it. */
bool ringbuf_push(struct ringbuf *rb, const void *data);

/* Consumer side only. Copies slot_size bytes out of the buffer into data.
 * Returns false without blocking if the buffer is empty, or (in
 * overwrite_on_full mode only) if the producer is evicting this exact
 * slot right now, or evicted this message while we were reading it --
 * the caller's normal retry handles it. On a false return the contents
 * of data are unspecified and must not be used. */
bool ringbuf_pop(struct ringbuf *rb, void *data);

size_t ringbuf_capacity(const struct ringbuf *rb);
size_t ringbuf_slot_size(const struct ringbuf *rb);

/* Number of messages evicted by ringbuf_push before being read. Always 0
 * if the buffer was created with overwrite_on_full = false. */
size_t ringbuf_dropped_count(const struct ringbuf *rb);

/* Diagnostic only: result may already be stale by the time it's read,
 * since the other side can concurrently push/pop. */
bool ringbuf_is_empty_approx(const struct ringbuf *rb);
bool ringbuf_is_full_approx(const struct ringbuf *rb);

#ifdef RINGBUF_TEST_HOOKS
/* Test-only, compiled in only under -DRINGBUF_TEST_HOOKS.
 *
 * Repositions both internal counters to `start`, leaving the buffer empty.
 * This exists so tests can exercise the point where the counters overflow
 * SIZE_MAX, which would otherwise require 2^64 pushes to reach.
 *
 * Preconditions: the buffer must be empty, and no producer or consumer may
 * be running concurrently. */
void ringbuf_test_seed_counters(struct ringbuf *rb, size_t start);
#endif

#endif /* RINGBUF_H */
