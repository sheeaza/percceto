#define _POSIX_C_SOURCE 200112L

#include "ringbuf.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct ringbuf {
    size_t capacity;
    size_t mask;
    size_t slot_size;
    char *buffer;
    bool overwrite_on_full;

    /* One lock per slot, only allocated when overwrite_on_full is set.
     * Guards the single physical slot where an in-flight ringbuf_pop can
     * overlap with a ringbuf_push eviction (see design notes in the repo
     * plan). 0 = free, 1 = held. */
    atomic_int *slot_lock;
    atomic_size_t dropped_count;

    _Alignas(64) atomic_size_t write_idx; /* producer-owned */
    size_t cached_read;                   /* producer's private cache of read_idx */

    _Alignas(64) atomic_size_t read_idx;  /* consumer-owned */
    size_t cached_write;                  /* consumer's private cache of write_idx */
};

static size_t next_pow2(size_t v) {
    if (v < 2)
        return 2;
    size_t p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

ringbuf_t *ringbuf_create(size_t slot_size, size_t min_capacity, bool overwrite_on_full) {
    if (slot_size == 0)
        return NULL;

    size_t capacity = next_pow2(min_capacity);

    ringbuf_t *rb = NULL;
    if (posix_memalign((void **)&rb, 64, sizeof(*rb)) != 0)
        return NULL;

    char *buffer = NULL;
    if (posix_memalign((void **)&buffer, 64, capacity * slot_size) != 0) {
        free(rb);
        return NULL;
    }

    atomic_int *slot_lock = NULL;
    if (overwrite_on_full) {
        slot_lock = malloc(capacity * sizeof(*slot_lock));
        if (!slot_lock) {
            free(buffer);
            free(rb);
            return NULL;
        }
        for (size_t i = 0; i < capacity; i++)
            atomic_init(&slot_lock[i], 0);
    }

    rb->capacity = capacity;
    rb->mask = capacity - 1;
    rb->slot_size = slot_size;
    rb->buffer = buffer;
    rb->overwrite_on_full = overwrite_on_full;
    rb->slot_lock = slot_lock;
    atomic_init(&rb->dropped_count, (size_t)0);
    atomic_init(&rb->write_idx, (size_t)0);
    rb->cached_read = 0;
    atomic_init(&rb->read_idx, (size_t)0);
    rb->cached_write = 0;

    return rb;
}

void ringbuf_destroy(ringbuf_t *rb) {
    if (!rb)
        return;
    free(rb->slot_lock);
    free(rb->buffer);
    free(rb);
}

bool ringbuf_push(ringbuf_t *rb, const void *data) {
    size_t write_idx = atomic_load_explicit(&rb->write_idx, memory_order_relaxed);
    size_t next = write_idx + 1;
    size_t idx = write_idx & rb->mask;

    if (next - rb->cached_read > rb->capacity) {
        rb->cached_read = atomic_load_explicit(&rb->read_idx, memory_order_acquire);
        if (next - rb->cached_read > rb->capacity) {
            if (!rb->overwrite_on_full)
                return false; /* genuinely full */

            int expected = 0;
            if (!atomic_compare_exchange_strong_explicit(
                    &rb->slot_lock[idx], &expected, 1,
                    memory_order_acquire, memory_order_relaxed))
                return false; /* consumer is mid-read of this exact slot */

            size_t fresh_read = atomic_load_explicit(&rb->read_idx, memory_order_acquire);
            if (next - fresh_read > rb->capacity) {
                /* Still full, so claim the eviction of message `fresh_read`
                 * with a CAS. Both we and the consumer advance read_idx only
                 * via CAS(x -> x+1), so exactly one of us can ever retire a
                 * given message: it is counted as dropped or as received,
                 * never both, and read_idx can never move backwards. */
                size_t expected_read = fresh_read;
                if (atomic_compare_exchange_strong_explicit(
                        &rb->read_idx, &expected_read, fresh_read + 1,
                        memory_order_acq_rel, memory_order_acquire)) {
                    atomic_fetch_add_explicit(&rb->dropped_count, 1, memory_order_relaxed);
                    rb->cached_read = fresh_read + 1;
                } else {
                    /* Consumer retired it first, so no eviction happened.
                     * Its advance also means we are no longer full. */
                    rb->cached_read = expected_read;
                }
            } else {
                /* consumer already vacated this slot normally in the meantime */
                rb->cached_read = fresh_read;
            }

            memcpy(rb->buffer + idx * rb->slot_size, data, rb->slot_size);
            atomic_store_explicit(&rb->slot_lock[idx], 0, memory_order_release);
            atomic_store_explicit(&rb->write_idx, next, memory_order_release);
            return true;
        }
    }

    memcpy(rb->buffer + idx * rb->slot_size, data, rb->slot_size);
    atomic_store_explicit(&rb->write_idx, next, memory_order_release);
    return true;
}

bool ringbuf_pop(ringbuf_t *rb, void *data) {
    size_t read_idx = atomic_load_explicit(&rb->read_idx, memory_order_relaxed);

    /* Signed difference so this stays correct when the counters wrap past
     * SIZE_MAX; a plain `read_idx >= cached_write` would read as "empty"
     * forever once write_idx wrapped and read_idx had not yet. A negative
     * difference just means cached_write is stale (the producer can advance
     * read_idx in overwrite mode), which is handled by the same refresh. */
    if ((ptrdiff_t)(rb->cached_write - read_idx) <= 0) {
        rb->cached_write = atomic_load_explicit(&rb->write_idx, memory_order_acquire);
        if ((ptrdiff_t)(rb->cached_write - read_idx) <= 0)
            return false; /* genuinely empty */
    }

    size_t idx = read_idx & rb->mask;

    if (rb->overwrite_on_full) {
        int expected = 0;
        if (!atomic_compare_exchange_strong_explicit(
                &rb->slot_lock[idx], &expected, 1,
                memory_order_acquire, memory_order_relaxed))
            return false; /* producer is evicting this exact slot right now */

        memcpy(data, rb->buffer + idx * rb->slot_size, rb->slot_size);
        atomic_store_explicit(&rb->slot_lock[idx], 0, memory_order_release);

        /* read_idx has two writers in overwrite mode (us, and the producer
         * when it evicts), so claim our advance with a CAS rather than a
         * blind store -- a blind store built on the value we loaded above
         * could move read_idx backwards over an eviction that landed in
         * the meantime, causing the same message to be handed out twice. */
        size_t expected_read = read_idx;
        if (!atomic_compare_exchange_strong_explicit(
                &rb->read_idx, &expected_read, read_idx + 1,
                memory_order_release, memory_order_relaxed))
            return false; /* producer evicted this message out from under us */

        return true;
    }

    memcpy(data, rb->buffer + idx * rb->slot_size, rb->slot_size);
    atomic_store_explicit(&rb->read_idx, read_idx + 1, memory_order_release);
    return true;
}

size_t ringbuf_capacity(const ringbuf_t *rb) {
    return rb->capacity;
}

size_t ringbuf_slot_size(const ringbuf_t *rb) {
    return rb->slot_size;
}

size_t ringbuf_dropped_count(const ringbuf_t *rb) {
    return atomic_load_explicit(&rb->dropped_count, memory_order_relaxed);
}

bool ringbuf_is_empty_approx(const ringbuf_t *rb) {
    size_t w = atomic_load_explicit(&rb->write_idx, memory_order_acquire);
    size_t r = atomic_load_explicit(&rb->read_idx, memory_order_acquire);
    return (ptrdiff_t)(w - r) <= 0;
}

bool ringbuf_is_full_approx(const ringbuf_t *rb) {
    size_t w = atomic_load_explicit(&rb->write_idx, memory_order_acquire);
    size_t r = atomic_load_explicit(&rb->read_idx, memory_order_acquire);
    return (ptrdiff_t)(w - r) >= (ptrdiff_t)rb->capacity;
}

#ifdef RINGBUF_TEST_HOOKS
void ringbuf_test_seed_counters(ringbuf_t *rb, size_t start) {
    atomic_store_explicit(&rb->write_idx, start, memory_order_relaxed);
    atomic_store_explicit(&rb->read_idx, start, memory_order_relaxed);
    rb->cached_read = start;
    rb->cached_write = start;
}
#endif
