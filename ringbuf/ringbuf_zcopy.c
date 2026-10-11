#define _POSIX_C_SOURCE 200112L

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ringbuf_zcopy.h"

struct ringbuf_zcopy {
    size_t capacity;
    size_t mask;
    size_t slot_size;
    bool overwrite_on_full;

    /* One stamp per slot: the publication state of slot i, and what replaces
     * write_idx as the readiness marker.
     *
     * This is the one structural departure from ringbuf_mutex.c, and the
     * split push forces it. There, write_idx IS the readiness marker: the
     * release store that advances it past a slot is the same event that
     * publishes the message, so "stored" is exactly the prefix below
     * write_idx. Here reservations are handed out in ticket order but
     * SUBMITTED in any order, so stored slots no longer form a prefix -- a
     * single monotonic counter cannot describe a ring with holes in it. The
     * state has to go per slot.
     *
     * Initialized to stamp[i] = i. For a slot holding ticket w the value
     * means:
     *
     *   stamp == w      reserved for ticket w: either free, or claimed by a
     *                   reservation that has not submitted yet
     *   stamp == w + 1  submitted, holds ticket w's message
     *
     * so a slot's life cycle is i -> i+1 -> i+capacity -> i+capacity+1 ...,
     * gaining `capacity` each lap. Compared against a ticket as an exact
     * equality or a signed difference, never as an absolute magnitude, so it
     * survives the SIZE_MAX wrap like every other counter here.
     *
     * Note what the encoding deliberately does NOT distinguish: "free" and
     * "reserved but unsubmitted" are the same value, w. Nothing needs to tell
     * them apart, because the question is always asked about a slot's
     * PREVIOUS lap -- reserve() is about to claim ticket w and asks after
     * ticket w - capacity, where the two readings differ. See
     * ringbuf_zcopy_reserve.
     *
     * This also subsumes the fullness and emptiness checks, which is why
     * there is no cached_read or cached_write in this struct -- unlike
     * ringbuf_mutex.c, whose producer compares the two counters directly. */
    atomic_size_t *stamp;

    atomic_size_t dropped_count;

    /* Single-writer: only the producer ever stores to write_idx, in either
     * mode, exactly as in ringbuf_mutex.c. The split push does not change
     * that -- reserve() is single-caller, and submit() touches only the
     * stamp, never this counter.
     *
     * Relaxed throughout, which is the one ordering that got WEAKER than
     * ringbuf_mutex.c's. There the release store on write_idx is what
     * publishes the message; here publication is the stamp's job, and nothing
     * reads write_idx to decide a slot is ready: only the producer itself,
     * the locked eviction path, and the two advisory *_approx helpers. */
    _Alignas(64) atomic_size_t write_idx;

    /* Retirement counter, and the one place the clean SPSC split breaks: in
     * overwrite mode the producer advances it too, when it evicts, so it has
     * two writers.
     *
     * `lock` is what reconciles them, held by every pop and by reserve's
     * eviction branch only -- the arrangement from ringbuf_mutex.c, kept
     * unchanged because a single producer still makes it sufficient. In
     * particular read_idx needs no CAS: a plain store under this lock is
     * already single-valued. Unused, and not even initialized, in lossless
     * mode.
     *
     * Still atomic rather than a plain size_t, because the *_approx helpers
     * read it without locking.
     *
     * The lock sits on this cache line rather than its own: an eviction takes
     * it and then immediately stores read_idx, so pairing them costs one line
     * transfer instead of two. The line is still split off from write_idx,
     * which the producer owns. */
    _Alignas(64) pthread_mutex_t lock;
    atomic_size_t read_idx;

    /* Slot data, then the stamp array. One allocation, so create/destroy stay
     * a single posix_memalign/free. A reservation's data pointer aims
     * directly into the slot region here, which is the whole point of the
     * variant: the caller fills the ring in place. */
    _Alignas(64) char tail[];
};

/**
 * next_pow2 - round up to a power of two
 * @v: value to round
 *
 * Return: the smallest power of two >= @v, but never less than 2 -- the
 * mask-based slot indexing needs both properties, and so does the stamp
 * encoding (see ringbuf_zcopy_pop).
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
 * ringbuf_zcopy_create - allocate a ring buffer
 * @slot_size: size of each slot in bytes
 * @min_capacity: lower bound on the slot count
 * @overwrite_on_full: evict the oldest slot instead of failing on full
 *
 * See ringbuf_zcopy.h for the caller-visible contract. One allocation covers
 * the header, the slot data and the stamps. The mutex is initialized only in
 * overwrite mode, which is the only mode that has two writers to reconcile.
 *
 * Return: the new buffer, or NULL on allocation failure, mutex-init failure,
 * or invalid arguments.
 */
struct ringbuf_zcopy *ringbuf_zcopy_create(size_t slot_size,
                                           size_t min_capacity,
                                           bool overwrite_on_full)
{
    if (slot_size == 0)
        return NULL;

    size_t capacity = next_pow2(min_capacity);

    /* Every step is overflow-checked because slot_size and min_capacity are
     * caller-supplied; an unchecked capacity*slot_size could wrap and
     * under-allocate, turning every later slot write -- here, every write a
     * CALLER makes through a reservation -- into a heap overflow. */
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

    size_t total =
        add_checked(offsetof(struct ringbuf_zcopy, tail), tail_bytes);
    if (total == 0)
        return NULL;

    struct ringbuf_zcopy *rb = NULL;
    if (posix_memalign((void **)&rb, _Alignof(struct ringbuf_zcopy), total) !=
        0)
        return NULL;

    /* Only overwrite mode has two writers to reconcile, so only it pays for
     * a mutex. ringbuf_zcopy_destroy mirrors this guard. */
    if (overwrite_on_full && pthread_mutex_init(&rb->lock, NULL) != 0) {
        free(rb);
        return NULL;
    }

    rb->capacity = capacity;
    rb->mask = capacity - 1;
    rb->slot_size = slot_size;
    rb->overwrite_on_full = overwrite_on_full;
    rb->stamp = (atomic_size_t *)(void *)(rb->tail + stamp_offset);
    atomic_init(&rb->dropped_count, (size_t)0);
    atomic_init(&rb->write_idx, (size_t)0);
    atomic_init(&rb->read_idx, (size_t)0);

    /* stamp[i] = i marks every slot free and reserved for ticket i, which is
     * the first lap. */
    for (size_t i = 0; i < capacity; i++)
        atomic_init(&rb->stamp[i], i);

    return rb;
}

/**
 * ringbuf_zcopy_destroy - free a ring buffer
 * @rb: buffer from ringbuf_zcopy_create(), or NULL
 *
 * Destroys the mutex only if create initialized one. A single free(), since
 * the slots and stamps live in the same allocation as the header -- which is
 * also why no reservation may still be outstanding: its data pointer aims
 * into this block.
 */
void ringbuf_zcopy_destroy(struct ringbuf_zcopy *rb)
{
    if (!rb)
        return;
    if (rb->overwrite_on_full)
        pthread_mutex_destroy(&rb->lock);
    free(rb);
}

/**
 * ringbuf_zcopy_reserve - claim a slot to fill in place
 * @rb: the buffer
 * @rsv: handle to fill in, valid only on a true return
 *
 * Producer side only, single-caller. See ringbuf_zcopy.h for the
 * caller-visible contract. The hot path runs unlocked; only the eviction
 * branch takes the mutex, and it is the only path that can block.
 *
 * ORDERING, and the one part of this file with no counterpart in the other
 * variants. Everywhere else an acquire/release pair brackets a window inside
 * a single push; here the window spans threads, and it takes three edges:
 *
 *   1. the acquire load below, pairing with pop's release store that frees
 *      the slot -- so the consumer's copy-out is complete before the bytes
 *      are handed to a filler;
 *   2. the caller's own handoff of @rsv to whatever fills it (pthread_create,
 *      a work queue, ...), which is the caller's to supply, not ours;
 *   3. submit's release store on the stamp, pairing with pop's acquire load
 *      -- so the fill is visible before the consumer copies it out.
 *
 * Transitivity across all three is what makes the whole thing safe, and it
 * holds whether submit runs on the filler thread or back on this one after a
 * join.
 *
 * Return: true if a slot was claimed; false if the buffer is full (lossless
 * mode) or the slot it needs is reserved but unsubmitted (either mode).
 */
bool ringbuf_zcopy_reserve(struct ringbuf_zcopy *rb, struct ringbuf_rsv *rsv)
{
    size_t w = atomic_load_explicit(&rb->write_idx, memory_order_relaxed);
    size_t idx = w & rb->mask;

    /* Acquire, and edge 1 of the three above. Pairs with pop's release store
     * freeing this slot, so the consumer's finished read is visible before we
     * hand the pointer out. This is the ordering that does the real work on
     * the reserve side -- relaxing it lets a filler's writes float above the
     * check on a weak memory model. */
    size_t st = atomic_load_explicit(&rb->stamp[idx], memory_order_acquire);

    if (st != w) {
        /* Not free. The single producer is what keeps this exhaustive: slot
         * idx last held ticket p = w - capacity, so the stamp is either p (p
         * is reserved and NOT yet submitted) or p + 1 (p was submitted and is
         * still unread). Two states, no others.
         *
         * A stamp past w in particular is impossible, so there is no
         * staleness case to retry: only the holder of reservation w can
         * publish w, w is handed out by this function alone, and this
         * function is single-caller. Were there a second producer, w could be
         * stale the instant it was loaded and everything below would need a
         * retry loop. */
        if (st == w - rb->capacity) {
            /* A filler may be writing these bytes right now, so this slot
             * cannot be evicted, in either mode. Only the caller's own submit
             * can clear it. */
            return false;
        }

        /* st == w - capacity + 1: full of an unread message. */
        if (!rb->overwrite_on_full)
            return false; /* genuinely full */

        pthread_mutex_lock(&rb->lock);

        /* The stamp is re-read HERE, under the lock, rather than reusing the
         * value sampled before it -- the same trap as ringbuf_mutex.c's pop
         * and ringbuf_vslot.c's. While we waited, the consumer may have
         * received that message normally and freed the slot; evicting anyway
         * would count a drop for a message that was delivered and break
         * received + dropped == pushed.
         *
         * Relaxed: the producer touches read_idx and this stamp value only
         * under the lock, pop takes the same lock in overwrite mode, so the
         * unlock/lock edge already orders us. The acquire above is the access
         * that has no lock on its end. */
        st = atomic_load_explicit(&rb->stamp[idx], memory_order_relaxed);
        if (st != w) {
            /* Still unread, so retire it as dropped.
             *
             * read_idx is exactly w - capacity here, which is why eviction
             * never has to search for a victim or skip over reserved slots,
             * and it is the one place the single-producer premise does
             * visible work. Reservations are handed out in ticket order by
             * one thread, so every ticket older than w - capacity has already
             * been retired -- its slot could not have been re-reserved
             * otherwise -- while w - capacity itself is unread by the stamp
             * we just read. */
            atomic_store_explicit(&rb->read_idx, w - rb->capacity + 1,
                                  memory_order_relaxed);
            atomic_fetch_add_explicit(&rb->dropped_count, 1,
                                      memory_order_relaxed);

            /* LOAD-BEARING, and it looks redundant: we are about to reserve
             * this slot ourselves, so why mark it free first? Because the
             * stamp must name the ticket the slot now awaits. Left at
             * w - capacity + 1, it would be read next lap (ticket
             * w + capacity) as "w + capacity - capacity + 1", i.e. published
             * and evictable -- while our filler was still mid-write. The
             * invariant to keep: whenever reserve hands out a pointer, the
             * stamp for that slot reads exactly its ticket. */
            atomic_store_explicit(&rb->stamp[idx], w, memory_order_relaxed);
        }
        /* else: the consumer drained the slot while we waited on the lock, so
         * there was nothing to evict and nothing is counted. Its pop already
         * stamped the slot w and advanced read_idx. */

        pthread_mutex_unlock(&rb->lock);
    }

    rsv->data = rb->tail + idx * rb->slot_size;
    rsv->ticket = w;

    /* Relaxed, and deliberately not the release store ringbuf_mutex.c has at
     * this point: this does not publish anything. The slot is not ready until
     * submit stamps it, and no reader of write_idx treats it as readiness. */
    atomic_store_explicit(&rb->write_idx, w + 1, memory_order_relaxed);
    return true;
}

/**
 * ringbuf_zcopy_submit - publish a filled reservation to the consumer
 * @rb: the buffer
 * @rsv: handle from a successful ringbuf_zcopy_reserve()
 *
 * Callable from any thread. See ringbuf_zcopy.h for the caller-visible
 * contract.
 *
 * One release store, no lock, in either mode -- which is what lets several
 * fills proceed in parallel and submit independently. That it needs no lock
 * even in overwrite mode is provable rather than hopeful: an evicting reserve
 * only ever touches a slot whose stamp it has observed as submitted, and a
 * submitted ticket is by definition not this one, so no eviction can be
 * racing us here. Concurrent submits of distinct reservations touch distinct
 * slots, since at most `capacity` tickets can be outstanding.
 *
 * The release is edge 3 of the three in ringbuf_zcopy_reserve: it publishes
 * the caller's fill to pop's acquire load. ticket + 1 is what marks the slot
 * submitted and names the ticket it holds.
 */
void ringbuf_zcopy_submit(struct ringbuf_zcopy *rb,
                          const struct ringbuf_rsv *rsv)
{
    atomic_store_explicit(&rb->stamp[rsv->ticket & rb->mask], rsv->ticket + 1,
                          memory_order_release);
}

/**
 * ringbuf_zcopy_push - reserve, copy one message in, and submit
 * @rb: the buffer
 * @data: slot_size bytes to copy in
 *
 * Producer side only. See ringbuf_zcopy.h for the caller-visible contract,
 * including why this always succeeds in overwrite mode.
 *
 * Return: true if the message was stored; false only when the buffer is full
 * in lossless mode.
 */
bool ringbuf_zcopy_push(struct ringbuf_zcopy *rb, const void *data)
{
    struct ringbuf_rsv rsv;

    if (!ringbuf_zcopy_reserve(rb, &rsv))
        return false;

    memcpy(rsv.data, data, rb->slot_size);
    ringbuf_zcopy_submit(rb, &rsv);
    return true;
}

/**
 * take_submitted - copy out ticket @r if it has been submitted
 * @rb: the buffer
 * @data: destination for slot_size bytes
 * @r: the ticket the consumer is waiting for, i.e. the current read_idx
 *
 * The body shared by both of pop's modes, caller holding the lock in
 * overwrite mode and no lock in lossless mode. Does not touch read_idx; the
 * caller stores it with the ordering its mode needs.
 *
 * Return: true if a message was copied out; false if ticket @r is not
 * submitted yet, whether because the buffer is empty or because a filler is
 * still working on it.
 */
static bool take_submitted(struct ringbuf_zcopy *rb, void *data, size_t r)
{
    size_t idx = r & rb->mask;

    /* Acquire, and edge 3's other end: pairs with submit's release store, so
     * the caller's fill is visible before we copy it out. The stamp also
     * subsumes the empty check -- anything other than "submitted at our lap"
     * means there is nothing for us, whether because the ring is empty or
     * because ticket r is reserved and still being filled. Those two are
     * indistinguishable here, which is exactly why pop's false return does
     * not mean empty. */
    size_t st = atomic_load_explicit(&rb->stamp[idx], memory_order_acquire);
    if (st != r + 1)
        return false;

    memcpy(data, rb->tail + idx * rb->slot_size, rb->slot_size);

    /* Release: frees the slot for ticket r + capacity, and publishes that our
     * read of the bytes above is finished. This is edge 1's other end, the
     * store reserve's acquire load pairs with; relaxing it is the classic way
     * to get a torn message on a weak model.
     *
     * r + capacity, not r + 1: the stamp names the next ticket this slot will
     * hold. Distinguishable from r + 1 because capacity >= 2. */
    atomic_store_explicit(&rb->stamp[idx], r + rb->capacity,
                          memory_order_release);
    return true;
}

/**
 * ringbuf_zcopy_pop - copy one message out of the buffer
 * @rb: the buffer
 * @data: destination for slot_size bytes
 *
 * Consumer side only, single-caller. See ringbuf_zcopy.h for the
 * caller-visible contract, including why false does not mean empty here.
 *
 * Lossless mode takes no lock; overwrite mode holds the mutex across the
 * whole operation and re-reads read_idx under it.
 *
 * Single-caller, but NOT for ringbuf_mutex.c's reason: there it is the plain
 * non-atomic cached_write that two consumers would race on, and this struct
 * has no such cache -- the stamp subsumes the empty check. What makes it
 * single-caller here is read_idx itself, stored plainly by one writer, so two
 * concurrent poppers would both read the same r and deliver the same message
 * twice.
 *
 * Return: true if a message was copied out; false if the buffer is empty or
 * the oldest slot is still awaiting its submit.
 */
bool ringbuf_zcopy_pop(struct ringbuf_zcopy *rb, void *data)
{
    if (!rb->overwrite_on_full) {
        /* Lossless mode: read_idx has exactly one writer (us), and the
         * producer never reads it -- fullness is the stamp's job -- so this
         * is the plain lock-free path. */
        size_t r = atomic_load_explicit(&rb->read_idx, memory_order_relaxed);

        if (!take_submitted(rb, data, r))
            return false;

        /* Release is conservative here: the only unlocked reader of read_idx
         * in this mode is the advisory *_approx pair, since the stamp handles
         * everything load-bearing. Kept for symmetry with the slot-freeing
         * store in take_submitted, and because ringbuf_mutex.c's lossless pop
         * carries a release on this same store -- there it is load-bearing,
         * since its producer reads read_idx to test fullness. */
        atomic_store_explicit(&rb->read_idx, r + 1, memory_order_release);
        return true;
    }

    /* Overwrite mode: hold the lock across the whole operation, so an
     * evicting producer cannot free our slot mid-copy. */
    pthread_mutex_lock(&rb->lock);

    /* read_idx is re-read HERE, under the lock, rather than reusing a value
     * sampled before it -- but unlike ringbuf_mutex.c's pop, from which this
     * shape is inherited, it is NOT load-bearing here, and claiming otherwise
     * would be reasoning by analogy across two files that differ. Verified by
     * deliberately sampling before the lock: the whole suite still passes.
     *
     * What makes a stale index harmless is take_submitted's exact-equality
     * test on the stamp. Had the producer evicted message r while we waited,
     * it would have re-stamped that slot for ticket r + capacity, so the stamp
     * reads r + capacity or r + capacity + 1 -- never r + 1, since capacity is
     * at least 2. So a stale r can only produce a spurious false return, which
     * this variant's contract already permits, never a wrong message and never
     * a read_idx that moves backwards. ringbuf_mutex.c has no such guard: its
     * pop validates only the index against cached_write, so there a stale
     * value genuinely does hand out the refilled slot's new message.
     *
     * Kept anyway: it costs one relaxed load on a path that already holds the
     * lock, it keeps the family's pop paths looking alike, and it means
     * correctness here does not lean entirely on the stamp encoding. */
    size_t r = atomic_load_explicit(&rb->read_idx, memory_order_relaxed);

    if (!take_submitted(rb, data, r)) {
        pthread_mutex_unlock(&rb->lock);
        return false;
    }

    /* Relaxed, unlike the lossless path's: in this mode the producer reads
     * and writes read_idx only under this same lock, so the unlock/lock edge
     * already orders us against it. Every non-relaxed ordering in this file
     * has an unlocked access on at least one end -- the stamp's, which submit
     * and reserve touch without locking. */
    atomic_store_explicit(&rb->read_idx, r + 1, memory_order_relaxed);

    pthread_mutex_unlock(&rb->lock);
    return true;
}

/**
 * ringbuf_zcopy_capacity - slot count of the buffer
 * @rb: the buffer
 *
 * Return: the capacity chosen at create time.
 */
size_t ringbuf_zcopy_capacity(const struct ringbuf_zcopy *rb)
{
    return rb->capacity;
}

/**
 * ringbuf_zcopy_slot_size - size of one slot
 * @rb: the buffer
 *
 * Return: the slot size in bytes.
 */
size_t ringbuf_zcopy_slot_size(const struct ringbuf_zcopy *rb)
{
    return rb->slot_size;
}

/**
 * ringbuf_zcopy_dropped_count - messages evicted before they were read
 * @rb: the buffer
 *
 * Return: the eviction count; always 0 in lossless mode.
 */
size_t ringbuf_zcopy_dropped_count(const struct ringbuf_zcopy *rb)
{
    return atomic_load_explicit(&rb->dropped_count, memory_order_relaxed);
}

/**
 * ringbuf_zcopy_is_empty_approx - diagnostic emptiness check
 * @rb: the buffer
 *
 * Counts reservations rather than submitted messages, so an outstanding one
 * reads as non-empty while pop still returns false.
 *
 * Return: true if the buffer looked empty; may be stale on return.
 */
bool ringbuf_zcopy_is_empty_approx(const struct ringbuf_zcopy *rb)
{
    size_t w = atomic_load_explicit(&rb->write_idx, memory_order_acquire);
    size_t r = atomic_load_explicit(&rb->read_idx, memory_order_acquire);
    return (ptrdiff_t)(w - r) <= 0;
}

/**
 * ringbuf_zcopy_is_full_approx - diagnostic fullness check
 * @rb: the buffer
 *
 * Counts reservations, which do occupy their slots.
 *
 * Return: true if the buffer looked full; may be stale on return.
 */
bool ringbuf_zcopy_is_full_approx(const struct ringbuf_zcopy *rb)
{
    size_t w = atomic_load_explicit(&rb->write_idx, memory_order_acquire);
    size_t r = atomic_load_explicit(&rb->read_idx, memory_order_acquire);
    return (ptrdiff_t)(w - r) >= (ptrdiff_t)rb->capacity;
}

#ifdef RINGBUF_TEST_HOOKS
/**
 * ringbuf_zcopy_test_seed_counters - reposition the counters, tests only
 * @rb: the buffer, which must be empty and quiescent
 * @start: value to park both internal counters at
 *
 * Relaxed stores throughout, and no lock taken: the preconditions rule out
 * any concurrent producer or consumer to synchronize with.
 *
 * Unlike ringbuf_mutex.c's counterpart, this must also re-stamp every slot --
 * seeding the two counters is not enough. The stamps hold absolute ticket
 * numbers, so leaving them at their create-time values desynchronizes them
 * from the seeded counters: every slot then reads as belonging to another lap,
 * reserve reports the slot permanently unsubmitted, and pop never sees a
 * message. Verified by disabling the loop below -- all five wrap_* cases
 * fail, including the single-threaded one.
 */
void ringbuf_zcopy_test_seed_counters(struct ringbuf_zcopy *rb, size_t start)
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
