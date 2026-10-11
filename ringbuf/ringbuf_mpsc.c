#define _POSIX_C_SOURCE 200112L

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ringbuf_mpsc.h"

struct ringbuf_mpsc {
    size_t capacity;
    size_t mask;
    size_t slot_size;
    bool overwrite_on_full;

    /* One stamp per slot: the publication state of slot i, and the whole
     * reason this variant needs no read_idx CAS.
     *
     * Initialized to stamp[i] = i. For a slot holding ticket w the value
     * means:
     *
     *   stamp == w      free, reserved for the producer holding ticket w
     *   stamp == w + 1  published, holds ticket w's message
     *
     * so a slot's life cycle is i -> i+1 -> i+capacity -> i+capacity+1 ...,
     * gaining `capacity` each lap. Compared against a ticket as a signed
     * difference, never absolutely, so it survives the SIZE_MAX wrap like
     * every other counter here.
     *
     * This subsumes the fullness check: a producer compares stamp[w & mask]
     * with its own w, and a negative difference means a previous lap's
     * message is still unread. Producers therefore never read read_idx at
     * all, which is why there is no cached_read in this struct. */
    atomic_size_t *stamp;

    /* One lock per slot, only present when overwrite_on_full is set. Guards
     * the single physical slot where an in-flight ringbuf_mpsc_pop can
     * overlap with a ringbuf_mpsc_push eviction. 0 = free, 1 = held.
     *
     * The stamp CAS already decides WHO retires a message; this lock is
     * what keeps the two memcpys off the same bytes while that is settled.
     * Both are needed, and they do different jobs.
     *
     * Points into the tail after the stamps, or NULL in lossless mode. Kept
     * as a pointer rather than recomputed so the hot paths index it
     * directly. */
    atomic_int *slot_lock;
    atomic_size_t dropped_count;

    /* Multi-writer, unlike every other variant in this tree: each producer
     * claims a slot by winning CAS(write_idx: w -> w+1), which is what
     * serializes concurrent pushes. ringbuf.c can use a relaxed-load /
     * release-store pair here precisely because it forbids that. */
    _Alignas(64) atomic_size_t write_idx;

    /* Retirement counter: how far delivery has got, and where the consumer
     * looks next.
     *
     * Single-writer in lossless mode, where the consumer is the only thread
     * that retires anything and a plain release store suffices. In
     * overwrite mode every evicting producer writes it too, so there are
     * N+1 writers and both sides go through advance_read_idx().
     *
     * The stamp CAS decides WHO retires a given message; this counter is
     * what tells the consumer where the live window starts. Both are
     * needed: an eviction that bumped only the stamp would leave the
     * consumer waiting on a ticket that no longer exists in the ring.
     *
     * The consumer's exclusive right to deliver is what keeps
     * ringbuf_mpsc_pop single-caller: two poppers would both see the same
     * r and both copy the same slot. Producers only ever push read_idx
     * forward past messages they destroyed, never past a live one. */
    _Alignas(64) atomic_size_t read_idx;

    /* Slot data, then the stamp array, then (in overwrite mode) the
     * slot_lock array. One allocation, so create/destroy stay a single
     * posix_memalign/free. */
    _Alignas(64) char tail[];
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
 * align_up - round up to an alignment boundary
 * @v: value to round
 * @align: alignment, which must be a power of two
 *
 * Return: @v rounded up to a multiple of @align, or 0 on overflow.
 */
static size_t align_up(size_t v, size_t align)
{
    size_t sum = add_checked(v, align - 1);
    if (sum == 0)
        return 0;
    return sum & ~(align - 1);
}

/**
 * ringbuf_mpsc_create - allocate a ring buffer
 * @slot_size: size of each slot in bytes
 * @min_capacity: lower bound on the slot count
 * @overwrite_on_full: evict the oldest slot instead of failing on full
 *
 * See ringbuf_mpsc.h for the caller-visible contract. One allocation covers
 * the header, the slot data, the stamps, and (in overwrite mode) the
 * trailing lock array.
 *
 * Return: the new buffer, or NULL on allocation failure or invalid
 * arguments.
 */
struct ringbuf_mpsc *ringbuf_mpsc_create(size_t slot_size, size_t min_capacity,
                                         bool overwrite_on_full)
{
    if (slot_size == 0)
        return NULL;

    size_t capacity = next_pow2(min_capacity);

    /* Every step is overflow-checked because slot_size and min_capacity are
     * caller-supplied; an unchecked capacity*slot_size could wrap and
     * under-allocate, turning every later slot write into a heap
     * overflow. */
    if (slot_size > SIZE_MAX / capacity)
        return NULL;
    size_t data_bytes = capacity * slot_size;

    /* The stamp array follows the slot data, so its start has to be aligned
     * for atomic_size_t -- slot_size carries no such guarantee. */
    size_t stamp_offset = align_up(data_bytes, _Alignof(atomic_size_t));
    if (stamp_offset == 0)
        return NULL;
    if (capacity > SIZE_MAX / sizeof(atomic_size_t))
        return NULL;
    size_t tail_bytes =
        add_checked(stamp_offset, capacity * sizeof(atomic_size_t));
    if (tail_bytes == 0)
        return NULL;

    size_t lock_offset = 0;
    if (overwrite_on_full) {
        lock_offset = align_up(tail_bytes, _Alignof(atomic_int));
        if (lock_offset == 0)
            return NULL;
        if (capacity > SIZE_MAX / sizeof(atomic_int))
            return NULL;
        tail_bytes = add_checked(lock_offset, capacity * sizeof(atomic_int));
        if (tail_bytes == 0)
            return NULL;
    }

    size_t total = add_checked(offsetof(struct ringbuf_mpsc, tail), tail_bytes);
    if (total == 0)
        return NULL;

    struct ringbuf_mpsc *rb = NULL;
    if (posix_memalign((void **)&rb, _Alignof(struct ringbuf_mpsc), total) != 0)
        return NULL;

    rb->capacity = capacity;
    rb->mask = capacity - 1;
    rb->slot_size = slot_size;
    rb->overwrite_on_full = overwrite_on_full;
    rb->stamp = (atomic_size_t *)(void *)(rb->tail + stamp_offset);
    rb->slot_lock = overwrite_on_full ?
                        (atomic_int *)(void *)(rb->tail + lock_offset) :
                        NULL;
    atomic_init(&rb->dropped_count, (size_t)0);
    atomic_init(&rb->write_idx, (size_t)0);
    atomic_init(&rb->read_idx, (size_t)0);

    /* stamp[i] = i marks every slot free and reserved for ticket i, which
     * is the first lap. */
    for (size_t i = 0; i < capacity; i++)
        atomic_init(&rb->stamp[i], i);

    for (size_t i = 0; overwrite_on_full && i < capacity; i++)
        atomic_init(&rb->slot_lock[i], 0);

    return rb;
}

/**
 * ringbuf_mpsc_destroy - free a ring buffer
 * @rb: buffer from ringbuf_mpsc_create(), or NULL
 *
 * A single free(), since the slots, stamps and lock array live in the same
 * allocation as the header.
 */
void ringbuf_mpsc_destroy(struct ringbuf_mpsc *rb)
{
    free(rb);
}

/**
 * advance_read_idx - move read_idx forward to @target, never backward
 * @rb: the buffer
 * @target: the value to raise read_idx to
 *
 * Overwrite mode only. read_idx has two kinds of writer there -- the
 * consumer retiring a message by receiving it, and any producer retiring one
 * by evicting it -- so a blind store from either could move it backwards
 * over the other's advance and hand out a message twice.
 *
 * A store-max rather than a fetch-add: the two sides can retire the same
 * message (one of them loses the slot's stamp CAS), and the loser must not
 * double-count. Exits immediately once read_idx is already at or past
 * @target, which is the common case under contention.
 *
 * Signed difference, so this behaves across the SIZE_MAX wrap.
 */
static void advance_read_idx(struct ringbuf_mpsc *rb, size_t target)
{
    size_t cur = atomic_load_explicit(&rb->read_idx, memory_order_relaxed);

    while ((ptrdiff_t)(target - cur) > 0) {
        if (atomic_compare_exchange_weak_explicit(&rb->read_idx, &cur, target,
                                                  memory_order_release,
                                                  memory_order_relaxed))
            return;
    }
}

/**
 * push_lossless - claim a ticket and publish, no eviction
 * @rb: the buffer
 * @data: slot_size bytes to copy in
 *
 * The fully lock-free path, used when overwrite_on_full is false. Takes no
 * lock on either side: the slot a won ticket names is owned exclusively by
 * the winner until it publishes, so there is nothing to exclude.
 *
 * Return: true if the message was stored; false if the buffer is full.
 */
static bool push_lossless(struct ringbuf_mpsc *rb, const void *data)
{
    size_t w = atomic_load_explicit(&rb->write_idx, memory_order_relaxed);

    for (;;) {
        /* Acquire: pairs with the consumer's release store when it frees
         * this slot, so its finished read is visible before we overwrite
         * the bytes. This is the ordering that does the real work -- on a
         * weak memory model, relaxing it lets the memcpy below float above
         * the check. */
        size_t st = atomic_load_explicit(&rb->stamp[w & rb->mask],
                                         memory_order_acquire);
        ptrdiff_t diff = (ptrdiff_t)(st - w);

        if (diff == 0) {
            /* Slot is free and reserved for ticket w. Claim it; on failure
             * another producer took w and expected holds the fresh value,
             * so loop without re-loading. */
            if (atomic_compare_exchange_weak_explicit(&rb->write_idx, &w, w + 1,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed))
                break;
            continue;
        }

        if (diff < 0)
            return false; /* a previous lap's message is still unread */

        /* diff > 0: w is stale, another producer has already published
         * here. Re-read and try again. */
        w = atomic_load_explicit(&rb->write_idx, memory_order_relaxed);
    }

    memcpy(rb->tail + (w & rb->mask) * rb->slot_size, data, rb->slot_size);

    /* Release: publishes the bytes above to the consumer's acquire load.
     * w+1 is what marks the slot full and names the ticket it holds. */
    atomic_store_explicit(&rb->stamp[w & rb->mask], w + 1,
                          memory_order_release);
    return true;
}

/**
 * push_overwrite - claim a ticket, evicting the oldest message if needed
 * @rb: the buffer
 * @data: slot_size bytes to copy in
 *
 * Used when overwrite_on_full is true. Takes the target slot's lock before
 * claiming the ticket -- see the comment inline, the order is load-bearing.
 *
 * Return: true if the message was stored; false on transient lock
 * contention with an in-flight pop of the slot being evicted.
 */
static bool push_overwrite(struct ringbuf_mpsc *rb, const void *data)
{
    size_t w = atomic_load_explicit(&rb->write_idx, memory_order_relaxed);

    for (;;) {
        size_t idx = w & rb->mask;
        size_t st = atomic_load_explicit(&rb->stamp[idx], memory_order_acquire);
        ptrdiff_t diff = (ptrdiff_t)(st - w);

        if (diff > 0) {
            /* Stale w; another producer published here already. */
            w = atomic_load_explicit(&rb->write_idx, memory_order_relaxed);
            continue;
        }

        if (diff == 0) {
            /* Free slot, no eviction needed: same as the lossless path. */
            if (atomic_compare_exchange_weak_explicit(&rb->write_idx, &w, w + 1,
                                                      memory_order_relaxed,
                                                      memory_order_relaxed))
                break;
            continue;
        }

        /* Full: slot idx still holds the unread message from ticket
         * w - capacity, and we have to evict it.
         *
         * Take the lock BEFORE claiming the ticket. The order matters: a
         * won ticket is irrevocable, so if we claimed it first and then
         * failed to take the lock, we would have to either block or leave a
         * permanent hole in the sequence at w -- and the consumer waits for
         * w forever, wedging the buffer. Locking on a merely tentative w is
         * safe because the ticket CAS below still validates it. */
        int expected_lock = 0;
        if (!atomic_compare_exchange_strong_explicit(
                &rb->slot_lock[idx], &expected_lock, 1, memory_order_acquire,
                memory_order_relaxed))
            return false; /* consumer is mid-read of this exact slot */

        /* Retire the message this slot holds, counting it as dropped. The
         * consumer's receive path takes this same slot_lock before it
         * touches the stamp, so the two can never run concurrently on one
         * slot: the lock -- not this CAS -- is what makes a message counted
         * as dropped or as received and never both, which is where
         * received + dropped == pushed comes from.
         *
         * A CAS rather than a store anyway, because it also discriminates
         * the three states the stamp can be in at this point, and the
         * branches below need to tell them apart. Verified by replacing it
         * with a plain store: the suite still passes, confirming the
         * exclusion is the lock's doing. */
        size_t victim_published = w - rb->capacity + 1;
        size_t expected_stamp = victim_published;
        if (atomic_compare_exchange_strong_explicit(
                &rb->stamp[idx], &expected_stamp, w, memory_order_acq_rel,
                memory_order_acquire)) {
            atomic_fetch_add_explicit(&rb->dropped_count, 1,
                                      memory_order_relaxed);
            /* The victim is retired, so read_idx must move past it. Without
             * this the consumer waits forever on a ticket that no longer
             * exists anywhere in the ring. */
            advance_read_idx(rb, w - rb->capacity + 1);
        } else if ((ptrdiff_t)(expected_stamp - w) < 0) {
            /* Only one state gets here: stamp == w - capacity, meaning the
             * producer holding the PREVIOUS lap's ticket for this slot has
             * not published yet. There is no message to evict and nothing
             * to copy over. Back off rather than spin, so push stays
             * non-blocking. */
            atomic_store_explicit(&rb->slot_lock[idx], 0, memory_order_release);
            return false;
        }
        /* The remaining case -- the CAS failed with the slot already free --
         * means the consumer retired the victim first. No eviction
         * happened, the slot is ours to fill, and nothing is counted. */

        /* Now make the ticket ours. On failure another producer won w while
         * we held the lock, so our idx was the wrong slot: drop the lock and
         * retry with a fresh w. Any eviction we already counted stands --
         * that message is genuinely destroyed either way, and the ticket's
         * real winner fills the slot. */
        if (!atomic_compare_exchange_strong_explicit(&rb->write_idx, &w, w + 1,
                                                     memory_order_relaxed,
                                                     memory_order_relaxed)) {
            atomic_store_explicit(&rb->slot_lock[idx], 0, memory_order_release);
            continue;
        }

        memcpy(rb->tail + idx * rb->slot_size, data, rb->slot_size);
        atomic_store_explicit(&rb->stamp[idx], w + 1, memory_order_release);
        atomic_store_explicit(&rb->slot_lock[idx], 0, memory_order_release);
        return true;
    }

    memcpy(rb->tail + (w & rb->mask) * rb->slot_size, data, rb->slot_size);
    atomic_store_explicit(&rb->stamp[w & rb->mask], w + 1,
                          memory_order_release);
    return true;
}

/**
 * ringbuf_mpsc_push - copy one message into the buffer
 * @rb: the buffer
 * @data: slot_size bytes to copy in
 *
 * Callable from any number of threads. See ringbuf_mpsc.h for the
 * caller-visible contract.
 *
 * Return: true if the message was stored; false if the buffer is full
 * (lossless mode), or on transient slot-lock contention with an in-flight
 * pop (overwrite mode).
 */
bool ringbuf_mpsc_push(struct ringbuf_mpsc *rb, const void *data)
{
    if (rb->overwrite_on_full)
        return push_overwrite(rb, data);
    return push_lossless(rb, data);
}

/**
 * pop_overwrite - take the oldest message, racing a possible evictor
 * @rb: the buffer
 * @data: destination for slot_size bytes
 * @r: the current read_idx
 *
 * Return: true if a message was copied out; false on transient contention
 * with an evicting producer.
 */
static bool pop_overwrite(struct ringbuf_mpsc *rb, void *data, size_t r)
{
    size_t idx = r & rb->mask;

    int expected_lock = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &rb->slot_lock[idx], &expected_lock, 1, memory_order_acquire,
            memory_order_relaxed))
        return false; /* producer is evicting this exact slot right now */

    /* read_idx is re-read HERE, under the lock, and the operation is
     * abandoned if it moved. While we waited for the lock a producer may
     * have evicted this message, counted it, and started refilling the
     * slot; proceeding on the stale r would hand out that new message as if
     * it were the one we were promised. */
    if (atomic_load_explicit(&rb->read_idx, memory_order_acquire) != r) {
        atomic_store_explicit(&rb->slot_lock[idx], 0, memory_order_release);
        return false;
    }

    /* The stamp confirms the slot actually holds ticket r's message and the
     * producer has published it. */
    size_t st = atomic_load_explicit(&rb->stamp[idx], memory_order_acquire);
    if (st != r + 1) {
        atomic_store_explicit(&rb->slot_lock[idx], 0, memory_order_release);
        return false; /* evicted out from under us, or not yet published */
    }

    memcpy(data, rb->tail + idx * rb->slot_size, rb->slot_size);

    /* Commit the receive. The evicting producer in push_overwrite holds
     * this same slot_lock while it retires a message, so it cannot be
     * racing us here; this is a CAS mainly for symmetry with that path and
     * as a cheap assertion that the stamp is still what we validated
     * above. */
    size_t expected_stamp = r + 1;
    if (!atomic_compare_exchange_strong_explicit(
            &rb->stamp[idx], &expected_stamp, r + rb->capacity,
            memory_order_release, memory_order_relaxed)) {
        atomic_store_explicit(&rb->slot_lock[idx], 0, memory_order_release);
        return false;
    }

    /* Only reached when our CAS won, so read_idx never passes a message we
     * did not deliver. Store-max rather than a plain store because evicting
     * producers write this too -- see advance_read_idx. */
    advance_read_idx(rb, r + 1);
    atomic_store_explicit(&rb->slot_lock[idx], 0, memory_order_release);
    return true;
}

/**
 * ringbuf_mpsc_pop - copy one message out of the buffer
 * @rb: the buffer
 * @data: destination for slot_size bytes
 *
 * Consumer side only, single-caller. See ringbuf_mpsc.h for the
 * caller-visible contract, including why false does not mean empty here.
 *
 * Return: true if a message was copied out; false if empty, if the oldest
 * message is still mid-publish, or on transient contention with an evicting
 * producer (overwrite mode).
 */
bool ringbuf_mpsc_pop(struct ringbuf_mpsc *rb, void *data)
{
    size_t r = atomic_load_explicit(&rb->read_idx, memory_order_relaxed);

    if (rb->overwrite_on_full)
        return pop_overwrite(rb, data, r);

    size_t idx = r & rb->mask;

    /* Acquire: pairs with the producer's release store in push_lossless, so
     * the message bytes are visible before we copy them out. The stamp also
     * subsumes the empty check -- anything other than "published at our
     * lap" means there is nothing for us, whether because the ring is empty
     * or because the producer holding ticket r has not published yet. */
    size_t st = atomic_load_explicit(&rb->stamp[idx], memory_order_acquire);
    if (st != r + 1)
        return false;

    memcpy(data, rb->tail + idx * rb->slot_size, rb->slot_size);

    /* Release: frees the slot for the producer holding ticket r + capacity,
     * and publishes that our read of the bytes above is finished. Relaxing
     * this is the classic way to get a torn message on a weak model. */
    atomic_store_explicit(&rb->stamp[idx], r + rb->capacity,
                          memory_order_release);
    atomic_store_explicit(&rb->read_idx, r + 1, memory_order_release);
    return true;
}

/**
 * ringbuf_mpsc_capacity - slot count of the buffer
 * @rb: the buffer
 *
 * Return: the capacity chosen at create time.
 */
size_t ringbuf_mpsc_capacity(const struct ringbuf_mpsc *rb)
{
    return rb->capacity;
}

/**
 * ringbuf_mpsc_slot_size - size of one slot
 * @rb: the buffer
 *
 * Return: the slot size in bytes.
 */
size_t ringbuf_mpsc_slot_size(const struct ringbuf_mpsc *rb)
{
    return rb->slot_size;
}

/**
 * ringbuf_mpsc_dropped_count - messages evicted before they were read
 * @rb: the buffer
 *
 * Return: the eviction count; always 0 in lossless mode.
 */
size_t ringbuf_mpsc_dropped_count(const struct ringbuf_mpsc *rb)
{
    return atomic_load_explicit(&rb->dropped_count, memory_order_relaxed);
}

/**
 * ringbuf_mpsc_is_empty_approx - diagnostic emptiness check
 * @rb: the buffer
 *
 * Return: true if the buffer looked empty; may be stale on return.
 */
bool ringbuf_mpsc_is_empty_approx(const struct ringbuf_mpsc *rb)
{
    size_t w = atomic_load_explicit(&rb->write_idx, memory_order_acquire);
    size_t r = atomic_load_explicit(&rb->read_idx, memory_order_acquire);
    return (ptrdiff_t)(w - r) <= 0;
}

/**
 * ringbuf_mpsc_is_full_approx - diagnostic fullness check
 * @rb: the buffer
 *
 * Return: true if the buffer looked full; may be stale on return.
 */
bool ringbuf_mpsc_is_full_approx(const struct ringbuf_mpsc *rb)
{
    size_t w = atomic_load_explicit(&rb->write_idx, memory_order_acquire);
    size_t r = atomic_load_explicit(&rb->read_idx, memory_order_acquire);
    return (ptrdiff_t)(w - r) >= (ptrdiff_t)rb->capacity;
}

#ifdef RINGBUF_TEST_HOOKS
/**
 * ringbuf_mpsc_test_seed_counters - reposition the counters, for tests only
 * @rb: the buffer, which must be empty and quiescent
 * @start: value to park both internal counters at
 *
 * Relaxed stores throughout: the preconditions rule out any concurrent
 * producer or consumer to synchronize with.
 *
 * Unlike its ringbuf.c counterpart this must also re-stamp every slot. The
 * stamps hold absolute ticket numbers, so leaving them at their create-time
 * values desynchronizes them from the seeded counters: every slot then
 * reads as belonging to some other lap, push spins in its retry loop, and
 * even the single-threaded wrap_boundary case hangs rather than failing a
 * check. Verified by removing the loop below.
 */
void ringbuf_mpsc_test_seed_counters(struct ringbuf_mpsc *rb, size_t start)
{
    atomic_store_explicit(&rb->write_idx, start, memory_order_relaxed);
    atomic_store_explicit(&rb->read_idx, start, memory_order_relaxed);

    /* start is a multiple of neither capacity nor anything else in
     * particular, so slot i is reserved for the first ticket >= start that
     * maps to it. Deriving that from the masked offset keeps the invariant
     * stamp[t & mask] == t for the ticket t each slot awaits. */
    for (size_t i = 0; i < rb->capacity; i++) {
        size_t offset = (i - (start & rb->mask)) & rb->mask;
        atomic_store_explicit(&rb->stamp[i], start + offset,
                              memory_order_relaxed);
    }
}
#endif
