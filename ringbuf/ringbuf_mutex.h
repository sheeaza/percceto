#ifndef RINGBUF_MUTEX_H
#define RINGBUF_MUTEX_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Single-producer / single-consumer ring buffer, mutex variant.
 *
 * Same data structure and same external SPSC contract as ringbuf.h:
 * exactly one thread may call ringbuf_mutex_push(); exactly one (possibly
 * different) thread may call ringbuf_mutex_pop(). Calling either from more
 * than one thread concurrently is undefined behavior.
 *
 * The difference is internal, and it is confined to overwrite mode. There,
 * the producer also advances the read index when it evicts, so the index
 * has two writers. ringbuf.c reconciles them lock-free, with a per-slot
 * spinlock array plus a CAS on the read index from both sides; the cost is
 * that both push and pop can return false on transient contention, so
 * every caller needs a retry loop.
 *
 * This variant reconciles them with one internal mutex instead. That is
 * easier to read, and it removes the spurious failures from the API (see
 * the two functions below). The trade is real and deliberate:
 *
 *   - ringbuf_mutex_push may BLOCK, where ringbuf_push never does.
 *   - It is therefore not lock-free and not async-signal-safe. Do not call
 *     it from a signal handler or any other context that cannot afford to
 *     wait on the consumer.
 *
 * Prefer ringbuf.h when either of those matters; prefer this one otherwise.
 *
 * Lossless mode (overwrite_on_full = false) has only one writer to the read
 * index, so it needs no mutex and none is taken. Both variants are
 * identical and fully lock-free in that mode.
 */

struct ringbuf_mutex;

/* Creates a ring buffer holding slots of slot_size bytes, with capacity
 * rounded up to the next power of two >= min_capacity (minimum 2).
 *
 * If overwrite_on_full is true, ringbuf_mutex_push never fails: it instead
 * evicts the oldest unread slot (see ringbuf_mutex_push). If false,
 * ringbuf_mutex_push fails on full.
 *
 * Returns NULL on allocation failure, mutex-init failure, or invalid
 * arguments. */
struct ringbuf_mutex *ringbuf_mutex_create(size_t slot_size,
                                           size_t min_capacity,
                                           bool overwrite_on_full);

/* Frees a ring buffer created by ringbuf_mutex_create. */
void ringbuf_mutex_destroy(struct ringbuf_mutex *rb);

/* Producer side only. Copies slot_size bytes from data into the buffer.
 *
 * If the buffer was created with overwrite_on_full = false: returns false
 * without blocking if the buffer is full.
 *
 * If created with overwrite_on_full = true: ALWAYS returns true. On full it
 * evicts the oldest unread slot and counts it in
 * ringbuf_mutex_dropped_count. Unlike ringbuf_push there is no transient
 * false return to retry -- but the eviction may block briefly if the
 * consumer is mid-read of that exact slot. */
bool ringbuf_mutex_push(struct ringbuf_mutex *rb, const void *data);

/* Consumer side only. Copies slot_size bytes out of the buffer into data.
 *
 * Returns false without blocking if, and only if, the buffer is empty --
 * in either mode. Unlike ringbuf_pop there is no transient false return
 * from losing a race with an evicting producer; in overwrite mode this may
 * block briefly on such a producer instead, and then either returns a
 * message or reports empty. On a false return the contents of data are
 * unspecified and must not be used. */
bool ringbuf_mutex_pop(struct ringbuf_mutex *rb, void *data);

size_t ringbuf_mutex_capacity(const struct ringbuf_mutex *rb);
size_t ringbuf_mutex_slot_size(const struct ringbuf_mutex *rb);

/* Number of messages evicted by ringbuf_mutex_push before being read.
 * Always 0 if the buffer was created with overwrite_on_full = false. */
size_t ringbuf_mutex_dropped_count(const struct ringbuf_mutex *rb);

/* Diagnostic only: result may already be stale by the time it's read,
 * since the other side can concurrently push/pop. */
bool ringbuf_mutex_is_empty_approx(const struct ringbuf_mutex *rb);
bool ringbuf_mutex_is_full_approx(const struct ringbuf_mutex *rb);

#ifdef RINGBUF_TEST_HOOKS
/* Test-only, compiled in only under -DRINGBUF_TEST_HOOKS.
 *
 * Repositions both internal counters to `start`, leaving the buffer empty.
 * This exists so tests can exercise the point where the counters overflow
 * SIZE_MAX, which would otherwise require 2^64 pushes to reach.
 *
 * Preconditions: the buffer must be empty, and no producer or consumer may
 * be running concurrently. */
void ringbuf_mutex_test_seed_counters(struct ringbuf_mutex *rb, size_t start);
#endif

#endif /* RINGBUF_MUTEX_H */
