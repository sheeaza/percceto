#define _POSIX_C_SOURCE 200112L

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ringbuf_mutex.h"

struct ringbuf_mutex {
    size_t capacity;
    size_t mask;
    size_t slot_size;
    bool overwrite_on_full;

    atomic_size_t dropped_count;

    /* Single-writer: only the producer ever stores to write_idx, in either
     * mode, so a cheap relaxed-load / release-store pair is enough. */
    _Alignas(64) atomic_size_t write_idx;
    size_t cached_read; /* producer-private cache of read_idx */

    /* Retirement counter, and the one place the clean SPSC split breaks:
     * in overwrite mode the producer advances it too, when it evicts.
     *
     * `lock` is what reconciles those two writers -- see the file-level
     * comment on ringbuf_mutex_push. It is held by every pop, and by the
     * eviction branch of push. Unused, and not even initialized, in
     * lossless mode.
     *
     * Still atomic rather than a plain size_t guarded by the lock, because
     * push's fast path reads it without locking. That one unlocked reader
     * is why pop's store carries release and push's fullness load carries
     * acquire; accesses with the lock on both ends are relaxed.
     *
     * The lock sits on this cache line rather than its own: an eviction
     * takes it and then immediately stores read_idx, so pairing them costs
     * one line transfer instead of two. The line is still split off from
     * write_idx, which the producer owns. */
    _Alignas(64) pthread_mutex_t lock;
    atomic_size_t read_idx;

    /* Consumer-private cache of write_idx. Plain size_t, deliberately: the
     * producer never touches it. This is what keeps ringbuf_mutex_pop
     * single-caller even though read_idx itself is now mutex-guarded --
     * two concurrent poppers would race here regardless. (In overwrite
     * mode it happens to be touched only under the lock, but the lock is a
     * producer-vs-consumer lock, not a consumer-vs-consumer one.) */
    size_t cached_write;

    /* Slot data. Lives in this one allocation, so the control block and the
     * slots it describes share cache lines at the boundary instead of
     * sitting in unrelated heap regions, and create/destroy are a single
     * malloc/free. Unlike ringbuf.c there is no trailing lock array -- the
     * single mutex above replaces it. */
    _Alignas(64) char slots[];
};

/**
 * next_pow2 - round up to a power of two
 * @v: value to round
 *
 * Return: the smallest power of two >= @v, but never less than 2 -- the
 * mask-based slot indexing needs both properties.
 */
static size_t next_pow2(size_t v)
{
    if (v < 2)
        return 2;
    size_t p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

/**
 * add_checked - add two sizes, detecting overflow
 * @a: first addend
 * @b: second addend
 *
 * Return: @a + @b, or 0 on overflow. 0 is never a legitimate total here
 * (the header alone is non-empty), so callers treat it as failure.
 */
static size_t add_checked(size_t a, size_t b)
{
    if (a > SIZE_MAX - b)
        return 0;
    return a + b;
}

/**
 * ringbuf_mutex_create - allocate a ring buffer
 * @slot_size: size of each slot in bytes
 * @min_capacity: lower bound on the slot count
 * @overwrite_on_full: evict the oldest slot instead of failing on full
 *
 * See ringbuf_mutex.h for the caller-visible contract. One allocation
 * covers the header and the slot data. The mutex is initialized only in
 * overwrite mode, which is the only mode that has two writers to
 * reconcile.
 *
 * Return: the new buffer, or NULL on allocation failure, mutex-init
 * failure, or invalid arguments.
 */
struct ringbuf_mutex *ringbuf_mutex_create(size_t slot_size,
                                           size_t min_capacity,
                                           bool overwrite_on_full)
{
    if (slot_size == 0)
        return NULL;

    size_t capacity = next_pow2(min_capacity);

    /* One allocation: header, then capacity*slot_size bytes of slots. Both
     * steps are overflow-checked because slot_size and min_capacity are
     * caller-supplied; an unchecked capacity*slot_size could wrap and
     * under-allocate, turning every later slot write into a heap
     * overflow. */
    if (slot_size > SIZE_MAX / capacity)
        return NULL;
    size_t data_bytes = capacity * slot_size;

    size_t total =
        add_checked(offsetof(struct ringbuf_mutex, slots), data_bytes);
    if (total == 0)
        return NULL;

    struct ringbuf_mutex *rb = NULL;
    if (posix_memalign((void **)&rb, _Alignof(struct ringbuf_mutex), total) !=
        0)
        return NULL;

    /* Only overwrite mode has two writers to reconcile, so only it pays for
     * a mutex. ringbuf_mutex_destroy mirrors this guard. */
    if (overwrite_on_full && pthread_mutex_init(&rb->lock, NULL) != 0) {
        free(rb);
        return NULL;
    }

    rb->capacity = capacity;
    rb->mask = capacity - 1;
    rb->slot_size = slot_size;
    rb->overwrite_on_full = overwrite_on_full;
    atomic_init(&rb->dropped_count, (size_t)0);
    atomic_init(&rb->write_idx, (size_t)0);
    rb->cached_read = 0;
    atomic_init(&rb->read_idx, (size_t)0);
    rb->cached_write = 0;

    return rb;
}

/**
 * ringbuf_mutex_destroy - free a ring buffer
 * @rb: buffer from ringbuf_mutex_create(), or NULL
 *
 * Destroys the mutex only if create initialized one; the slots need no
 * separate free, as they live in the same allocation as the header.
 */
void ringbuf_mutex_destroy(struct ringbuf_mutex *rb)
{
    if (!rb)
        return;
    if (rb->overwrite_on_full)
        pthread_mutex_destroy(&rb->lock);
    free(rb);
}

/**
 * ringbuf_mutex_push - copy one message into the buffer
 * @rb: the buffer
 * @data: slot_size bytes to copy in
 *
 * Producer side only. See ringbuf_mutex.h for the caller-visible contract.
 * The hot path runs unlocked; only the eviction branch takes the mutex,
 * and it is the only path that can block.
 *
 * Return: true if the message was stored; false only when the buffer is
 * full in lossless mode.
 */
bool ringbuf_mutex_push(struct ringbuf_mutex *rb, const void *data)
{
    size_t write_idx =
        atomic_load_explicit(&rb->write_idx, memory_order_relaxed);
    size_t next = write_idx + 1;
    size_t idx = write_idx & rb->mask;

    if (next - rb->cached_read > rb->capacity) {
        /* Acquire, and NOT redundant with the lock below. A mutex only
         * orders you against threads that take the same mutex, and this
         * load runs before we take it -- the fast path underneath never
         * takes it at all. So the consumer's unlock gives us no edge here;
         * this acquire pairing with pop's release store is what makes the
         * consumer's finished read visible before we overwrite the slot.
         *
         * That pair (this load, and the release store at the end of pop)
         * is the only ordering in this file doing real work. The two
         * stores inside the eviction branch below are relaxed, because
         * there both sides hold the lock. */
        rb->cached_read =
            atomic_load_explicit(&rb->read_idx, memory_order_acquire);
        if (next - rb->cached_read > rb->capacity) {
            if (!rb->overwrite_on_full)
                return false; /* genuinely full */

            /* Full, so our slot is the one the consumer may be reading.
             * Take the lock and evict under it; this is the only push path
             * that locks, and the only one that can block. */
            pthread_mutex_lock(&rb->lock);

            size_t fresh_read =
                atomic_load_explicit(&rb->read_idx, memory_order_relaxed);
            if (next - fresh_read > rb->capacity) {
                /* Still full. Retire message `fresh_read` as dropped. The
                 * lock means no pop can be mid-read of that slot, and no
                 * pop can observe read_idx between this store and the
                 * memcpy below -- so unlike ringbuf.c this needs no CAS to
                 * stay single-valued.
                 *
                 * Relaxed, unlike every other read_idx access here. This
                 * branch runs only in overwrite mode, where pop reads
                 * read_idx exclusively under this same lock, so the
                 * unlock/lock edge already orders us against it. See the
                 * note on the fullness load above for the accesses that do
                 * need explicit ordering. */
                atomic_store_explicit(&rb->read_idx, fresh_read + 1,
                                      memory_order_relaxed);
                atomic_fetch_add_explicit(&rb->dropped_count, 1,
                                          memory_order_relaxed);
                rb->cached_read = fresh_read + 1;
            } else {
                /* Consumer drained the slot normally while we were waiting
                 * on the lock, so there is nothing to evict. */
                rb->cached_read = fresh_read;
            }

            memcpy(rb->slots + idx * rb->slot_size, data, rb->slot_size);
            /* Relaxed, unlike the fast path's identical store below: this
             * branch is overwrite-mode-only, and there pop reads write_idx
             * under this same lock, so the unlock already publishes the
             * memcpy above. The fast path has no such edge and needs the
             * release. */
            atomic_store_explicit(&rb->write_idx, next, memory_order_relaxed);
            pthread_mutex_unlock(&rb->lock);
            return true;
        }
    }

    /* Not full, so write_idx - read_idx < capacity, which means
     * write_idx & mask != read_idx & mask: this slot cannot be the one an
     * in-flight pop is reading. The indices only alias when full, and that
     * is exactly the branch above. Hence no lock here -- this is the hot
     * path and it stays as cheap as the lock-free variant's. */
    memcpy(rb->slots + idx * rb->slot_size, data, rb->slot_size);
    atomic_store_explicit(&rb->write_idx, next, memory_order_release);
    return true;
}

/**
 * ringbuf_mutex_pop - copy one message out of the buffer
 * @rb: the buffer
 * @data: destination for slot_size bytes
 *
 * Consumer side only. See ringbuf_mutex.h for the caller-visible contract.
 * Lossless mode takes no lock; overwrite mode holds the mutex across the
 * whole operation and re-reads the indices under it.
 *
 * Return: true if a message was copied out; false if and only if the
 * buffer is empty.
 */
bool ringbuf_mutex_pop(struct ringbuf_mutex *rb, void *data)
{
    if (!rb->overwrite_on_full) {
        /* Lossless mode: read_idx has exactly one writer (us), so this is
         * the plain lock-free path, identical to ringbuf.c. */
        size_t read_idx =
            atomic_load_explicit(&rb->read_idx, memory_order_relaxed);

        /* Signed difference so this stays correct when the counters wrap
         * past SIZE_MAX; a plain `read_idx >= cached_write` would read as
         * "empty" forever once write_idx wrapped and read_idx had not. */
        if ((ptrdiff_t)(rb->cached_write - read_idx) <= 0) {
            rb->cached_write =
                atomic_load_explicit(&rb->write_idx, memory_order_acquire);
            if ((ptrdiff_t)(rb->cached_write - read_idx) <= 0)
                return false; /* genuinely empty */
        }

        memcpy(data, rb->slots + (read_idx & rb->mask) * rb->slot_size,
               rb->slot_size);
        atomic_store_explicit(&rb->read_idx, read_idx + 1,
                              memory_order_release);
        return true;
    }

    /* Overwrite mode: hold the lock across the whole operation, so an
     * evicting producer cannot touch our slot mid-copy. */
    pthread_mutex_lock(&rb->lock);

    /* Both indices are re-read HERE, under the lock, rather than reusing a
     * value sampled before it. This is load-bearing, and it is what
     * replaces ringbuf.c's CAS-failure path: while we waited on the lock
     * the producer may have evicted the very message we were about to
     * take, counted it in dropped_count, and refilled the slot. Acting on
     * a stale read_idx would hand out that new message as if it were the
     * evicted one and break received + dropped == pushed. Re-deriving
     * means we either take a message that is genuinely still ours or
     * report empty -- never a spurious failure. */
    size_t read_idx = atomic_load_explicit(&rb->read_idx, memory_order_relaxed);
    if ((ptrdiff_t)(rb->cached_write - read_idx) <= 0) {
        rb->cached_write =
            atomic_load_explicit(&rb->write_idx, memory_order_acquire);
        if ((ptrdiff_t)(rb->cached_write - read_idx) <= 0) {
            pthread_mutex_unlock(&rb->lock);
            return false; /* genuinely empty */
        }
    }

    memcpy(data, rb->slots + (read_idx & rb->mask) * rb->slot_size,
           rb->slot_size);

    /* Release, not relaxed, and the one store under this lock that is not:
     * push's fast path reads read_idx (see its fullness load) without ever
     * taking the mutex, so the unlock's release does not reach it. This
     * store is what tells that path our read of the slot is complete and
     * the slot is safe to overwrite. */
    atomic_store_explicit(&rb->read_idx, read_idx + 1, memory_order_release);

    pthread_mutex_unlock(&rb->lock);
    return true;
}

/**
 * ringbuf_mutex_capacity - slot count of the buffer
 * @rb: the buffer
 *
 * Return: the capacity chosen at create time.
 */
size_t ringbuf_mutex_capacity(const struct ringbuf_mutex *rb)
{
    return rb->capacity;
}

/**
 * ringbuf_mutex_slot_size - size of one slot
 * @rb: the buffer
 *
 * Return: the slot size in bytes.
 */
size_t ringbuf_mutex_slot_size(const struct ringbuf_mutex *rb)
{
    return rb->slot_size;
}

/**
 * ringbuf_mutex_dropped_count - messages evicted before they were read
 * @rb: the buffer
 *
 * Return: the eviction count; always 0 in lossless mode.
 */
size_t ringbuf_mutex_dropped_count(const struct ringbuf_mutex *rb)
{
    return atomic_load_explicit(&rb->dropped_count, memory_order_relaxed);
}

/**
 * ringbuf_mutex_is_empty_approx - diagnostic emptiness check
 * @rb: the buffer
 *
 * Return: true if the buffer looked empty; may be stale on return.
 */
bool ringbuf_mutex_is_empty_approx(const struct ringbuf_mutex *rb)
{
    size_t w = atomic_load_explicit(&rb->write_idx, memory_order_acquire);
    size_t r = atomic_load_explicit(&rb->read_idx, memory_order_acquire);
    return (ptrdiff_t)(w - r) <= 0;
}

/**
 * ringbuf_mutex_is_full_approx - diagnostic fullness check
 * @rb: the buffer
 *
 * Return: true if the buffer looked full; may be stale on return.
 */
bool ringbuf_mutex_is_full_approx(const struct ringbuf_mutex *rb)
{
    size_t w = atomic_load_explicit(&rb->write_idx, memory_order_acquire);
    size_t r = atomic_load_explicit(&rb->read_idx, memory_order_acquire);
    return (ptrdiff_t)(w - r) >= (ptrdiff_t)rb->capacity;
}

#ifdef RINGBUF_TEST_HOOKS
/**
 * ringbuf_mutex_test_seed_counters - reposition both counters, tests only
 * @rb: the buffer, which must be empty and quiescent
 * @start: value to park both internal counters at
 *
 * Relaxed stores throughout, and no lock taken: the preconditions rule out
 * any concurrent producer or consumer to synchronize with.
 */
void ringbuf_mutex_test_seed_counters(struct ringbuf_mutex *rb, size_t start)
{
    atomic_store_explicit(&rb->write_idx, start, memory_order_relaxed);
    atomic_store_explicit(&rb->read_idx, start, memory_order_relaxed);
    rb->cached_read = start;
    rb->cached_write = start;
}
#endif
