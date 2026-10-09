#define _POSIX_C_SOURCE 200112L

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ringbuf.h"

struct ringbuf {
    size_t capacity;
    size_t mask;
    size_t slot_size;
    bool overwrite_on_full;

    /* One lock per slot, only present when overwrite_on_full is set.
     * Guards the single physical slot where an in-flight ringbuf_pop can
     * overlap with a ringbuf_push eviction (see design notes in the repo
     * plan). 0 = free, 1 = held.
     *
     * Points into the tail after the slot data, or NULL in lossless mode.
     * Kept as a pointer rather than recomputed from capacity/slot_size so
     * the hot paths index it directly. */
    atomic_int *slot_lock;
    atomic_size_t dropped_count;

    /* Single-writer: only the producer ever stores to write_idx, in either
     * mode, which is why it keeps a cheap relaxed-load / release-store pair
     * rather than a CAS. */
    _Alignas(64) atomic_size_t write_idx;
    size_t cached_read; /* producer-private cache of read_idx */

    /* Retirement counter, and the one place the clean SPSC split breaks.
     *
     * In lossless mode the consumer is its only writer and a blind release
     * store suffices. In overwrite mode the producer writes it too, when it
     * evicts, so read_idx is multi-writer and both sides must advance it
     * via CAS(n -> n+1). Exactly one CAS per n can win, which is what makes
     * a message either received or dropped and never both.
     *
     * The cache-line padding is still worth it despite that contention:
     * evictions only occur when the buffer is full, so the common case
     * leaves this line to the consumer alone. */
    _Alignas(64) atomic_size_t read_idx;

    /* Consumer-private cache of write_idx. Plain size_t, deliberately: the
     * producer never touches it. This is also what makes ringbuf_pop
     * non-re-entrant -- two concurrent poppers would race here even though
     * read_idx itself is CAS-safe. */
    size_t cached_write;

    /* Slot data, then (in overwrite mode only) the slot_lock array. Both
     * live in this one allocation, so the control block and the slots it
     * describes share cache lines at the boundary instead of sitting in
     * unrelated heap regions, and create/destroy are a single
     * malloc/free. */
    _Alignas(64) char tail[];
};

static size_t next_pow2(size_t v)
{
    if (v < 2)
        return 2;
    size_t p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

/* a + b, or 0 on overflow. 0 is never a legitimate total here (the header
 * alone is non-empty), so callers treat it as failure. */
static size_t add_checked(size_t a, size_t b)
{
    if (a > SIZE_MAX - b)
        return 0;
    return a + b;
}

/* Rounds up to a power-of-two alignment, or 0 on overflow. */
static size_t align_up(size_t v, size_t align)
{
    size_t sum = add_checked(v, align - 1);
    if (sum == 0)
        return 0;
    return sum & ~(align - 1);
}

struct ringbuf *ringbuf_create(size_t slot_size, size_t min_capacity,
                               bool overwrite_on_full)
{
    if (slot_size == 0)
        return NULL;

    size_t capacity = next_pow2(min_capacity);

    /* One allocation: header, then capacity*slot_size bytes of slots, then
     * the optional lock array. Every step is overflow-checked because
     * slot_size and min_capacity are caller-supplied; an unchecked
     * capacity*slot_size could wrap and under-allocate the buffer. */
    if (slot_size > SIZE_MAX / capacity)
        return NULL;
    size_t data_bytes = capacity * slot_size;

    size_t tail_bytes = data_bytes;
    size_t lock_offset = 0;
    if (overwrite_on_full) {
        /* The lock array follows the slot data, so its start has to be
         * aligned for atomic_int -- slot_size carries no such guarantee. */
        lock_offset = align_up(data_bytes, _Alignof(atomic_int));
        if (lock_offset == 0)
            return NULL;
        if (capacity > SIZE_MAX / sizeof(atomic_int))
            return NULL;
        tail_bytes = add_checked(lock_offset, capacity * sizeof(atomic_int));
        if (tail_bytes == 0)
            return NULL;
    }

    size_t total = add_checked(offsetof(struct ringbuf, tail), tail_bytes);
    if (total == 0)
        return NULL;

    struct ringbuf *rb = NULL;
    if (posix_memalign((void **)&rb, _Alignof(struct ringbuf), total) != 0)
        return NULL;

    rb->capacity = capacity;
    rb->mask = capacity - 1;
    rb->slot_size = slot_size;
    rb->overwrite_on_full = overwrite_on_full;
    rb->slot_lock = overwrite_on_full ?
                        (atomic_int *)(void *)(rb->tail + lock_offset) :
                        NULL;
    atomic_init(&rb->dropped_count, (size_t)0);
    atomic_init(&rb->write_idx, (size_t)0);
    rb->cached_read = 0;
    atomic_init(&rb->read_idx, (size_t)0);
    rb->cached_write = 0;

    for (size_t i = 0; overwrite_on_full && i < capacity; i++)
        atomic_init(&rb->slot_lock[i], 0);

    return rb;
}

void ringbuf_destroy(struct ringbuf *rb)
{
    free(rb);
}

bool ringbuf_push(struct ringbuf *rb, const void *data)
{
    size_t write_idx =
        atomic_load_explicit(&rb->write_idx, memory_order_relaxed);
    size_t next = write_idx + 1;
    size_t idx = write_idx & rb->mask;

    if (next - rb->cached_read > rb->capacity) {
        rb->cached_read =
            atomic_load_explicit(&rb->read_idx, memory_order_acquire);
        if (next - rb->cached_read > rb->capacity) {
            if (!rb->overwrite_on_full)
                return false; /* genuinely full */

            int expected = 0;
            if (!atomic_compare_exchange_strong_explicit(
                    &rb->slot_lock[idx], &expected, 1, memory_order_acquire,
                    memory_order_relaxed))
                return false; /* consumer is mid-read of this exact slot */

            size_t fresh_read =
                atomic_load_explicit(&rb->read_idx, memory_order_acquire);
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
                    atomic_fetch_add_explicit(&rb->dropped_count, 1,
                                              memory_order_relaxed);
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

            memcpy(rb->tail + idx * rb->slot_size, data, rb->slot_size);
            atomic_store_explicit(&rb->slot_lock[idx], 0, memory_order_release);
            atomic_store_explicit(&rb->write_idx, next, memory_order_release);
            return true;
        }
    }

    memcpy(rb->tail + idx * rb->slot_size, data, rb->slot_size);
    atomic_store_explicit(&rb->write_idx, next, memory_order_release);
    return true;
}

bool ringbuf_pop(struct ringbuf *rb, void *data)
{
    size_t read_idx = atomic_load_explicit(&rb->read_idx, memory_order_relaxed);

    /* Signed difference so this stays correct when the counters wrap past
     * SIZE_MAX; a plain `read_idx >= cached_write` would read as "empty"
     * forever once write_idx wrapped and read_idx had not yet. A negative
     * difference just means cached_write is stale (the producer can advance
     * read_idx in overwrite mode), which is handled by the same refresh. */
    if ((ptrdiff_t)(rb->cached_write - read_idx) <= 0) {
        rb->cached_write =
            atomic_load_explicit(&rb->write_idx, memory_order_acquire);
        if ((ptrdiff_t)(rb->cached_write - read_idx) <= 0)
            return false; /* genuinely empty */
    }

    size_t idx = read_idx & rb->mask;

    if (rb->overwrite_on_full) {
        int expected = 0;
        if (!atomic_compare_exchange_strong_explicit(
                &rb->slot_lock[idx], &expected, 1, memory_order_acquire,
                memory_order_relaxed))
            return false; /* producer is evicting this exact slot right now */

        memcpy(data, rb->tail + idx * rb->slot_size, rb->slot_size);
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

    memcpy(data, rb->tail + idx * rb->slot_size, rb->slot_size);
    atomic_store_explicit(&rb->read_idx, read_idx + 1, memory_order_release);
    return true;
}

size_t ringbuf_capacity(const struct ringbuf *rb)
{
    return rb->capacity;
}

size_t ringbuf_slot_size(const struct ringbuf *rb)
{
    return rb->slot_size;
}

size_t ringbuf_dropped_count(const struct ringbuf *rb)
{
    return atomic_load_explicit(&rb->dropped_count, memory_order_relaxed);
}

bool ringbuf_is_empty_approx(const struct ringbuf *rb)
{
    size_t w = atomic_load_explicit(&rb->write_idx, memory_order_acquire);
    size_t r = atomic_load_explicit(&rb->read_idx, memory_order_acquire);
    return (ptrdiff_t)(w - r) <= 0;
}

bool ringbuf_is_full_approx(const struct ringbuf *rb)
{
    size_t w = atomic_load_explicit(&rb->write_idx, memory_order_acquire);
    size_t r = atomic_load_explicit(&rb->read_idx, memory_order_acquire);
    return (ptrdiff_t)(w - r) >= (ptrdiff_t)rb->capacity;
}

#ifdef RINGBUF_TEST_HOOKS
void ringbuf_test_seed_counters(struct ringbuf *rb, size_t start)
{
    atomic_store_explicit(&rb->write_idx, start, memory_order_relaxed);
    atomic_store_explicit(&rb->read_idx, start, memory_order_relaxed);
    rb->cached_read = start;
    rb->cached_write = start;
}
#endif
