#ifndef RINGBUF_MPSC_H
#define RINGBUF_MPSC_H

#include <stdatomic.h>
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
 *
 * SPLIT PUSH. push() is also available in two halves, for callers that would
 * rather write the payload straight into the slot than assemble it elsewhere
 * and have push() copy it in:
 *
 *	struct ringbuf_mpsc_rsv a, b;
 *	ringbuf_mpsc_reserve(rb, &a);	-- claim a slot, get a pointer to it
 *	ringbuf_mpsc_reserve(rb, &b);
 *
 *	fill(a.data);			-- these may run CONCURRENTLY, on
 *	fill(b.data);			-- other threads, as an in-flight
 *					   read(2), in a DMA engine, ...
 *
 *	ringbuf_mpsc_submit(rb, &b);	-- in ANY order
 *	ringbuf_mpsc_submit(rb, &a);
 *
 * Two things that buys: the fill writes straight into the slot, with no
 * staging buffer, and several fills can overlap each other instead of being
 * serialized by one thread's memcpy. Messages are delivered in RESERVATION
 * order regardless of submit order. push() is exactly reserve + memcpy +
 * submit, and is kept for callers that gain nothing from filling in place.
 *
 * EVERY SUCCESSFUL RESERVATION MUST BE SUBMITTED, exactly once. There is no
 * cancel. Abandoning a reservation stalls delivery permanently: the consumer
 * waits for that slot forever and the buffer wedges once the producers lap
 * the ring. On an error path, submit a sentinel payload the consumer can
 * recognize -- do not simply drop the handle.
 *
 * ringbuf_zcopy.h offers the same split push and is the variant to compare
 * against when choosing. The difference is not the operation but who
 * synchronizes the producers: it requires the CALLER to serialize every
 * reserve with a mutex of its own, whereas reserve() here is callable from any
 * number of threads concurrently and needs no caller lock, because the write
 * index is multi-writer and claims go through a validating ticket CAS. Pick
 * this one when producers must not block or you cannot hold a lock; pick
 * ringbuf_zcopy.h when you can hold a short lock and want its simpler
 * single-writer internals. BENCHMARK_ringbuf_mpsc_vs_ringbuf_zcopy.md measures
 * the two arrangements against each other.
 *
 * THE LAYOUT IS VISIBLE. struct ringbuf_mpsc is defined below rather than kept
 * opaque, so a caller can read a field directly, size the allocation itself, or
 * inline a hot path. That is a deliberate exception: ringbuf.h,
 * ringbuf_mutex.h and ringbuf_vslot.h keep theirs private. The fields remain
 * INTERNAL -- read them if it helps, but every WRITE belongs to the functions
 * here, which is where the memory ordering each field documents is actually
 * established. Nothing below changes behavior for a caller who ignores it.
 */

/**
 * struct ringbuf_mpsc - the ring buffer
 * @capacity: slot count, a power of two >= 2
 * @mask: @capacity - 1, for mapping a ticket to a slot
 * @slot_size: bytes per slot, as passed to ringbuf_mpsc_create()
 * @overwrite_on_full: the mode chosen at create time
 * @stamp: per-slot publication state, @capacity entries, pointing into @tail
 * @slot_lock: per-slot spinlock, @capacity entries, or NULL in lossless mode
 * @dropped_count: messages evicted before they were read
 * @write_idx: next ticket to hand out; multi-writer, see below
 * @read_idx: next ticket to deliver
 * @tail: slot data, then the stamps, then (overwrite mode) the lock array
 *
 * Defined here rather than kept opaque so callers can see the layout:
 * read a field directly, size the allocation themselves, or inline a hot
 * path. The fields are nonetheless INTERNAL -- read them if you like, but
 * every write belongs to the functions below, which is where the ordering
 * documented per field is established. Use ringbuf_mpsc_capacity() and
 * friends in portable code; they are not going to change.
 *
 * @tail is a flexible array member, so this struct CANNOT be a member of
 * another one (C11 6.7.2.1p3) -- deriving from it means wrapping the
 * pointer, or allocating the block yourself and treating the result as one
 * of these. In the latter case ringbuf_mpsc_create() is the specification of
 * what a valid block looks like: the tail needs @capacity * @slot_size
 * bytes, the stamp array aligned for atomic_size_t after them, then in
 * overwrite mode the lock array aligned for atomic_int, with stamp[i] = i
 * and slot_lock[i] = 0.
 */
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
 * struct ringbuf_mpsc_rsv - a claimed slot, awaiting submit
 * @data: slot_size writable bytes; fill these before submitting
 * @ticket: internal identifier of the claimed slot; do not interpret it
 *
 * Caller-allocated, and by value -- stack-allocate one per reservation,
 * rather than asking the buffer for one. Filled in by ringbuf_mpsc_reserve() and consumed by
 * ringbuf_mpsc_submit(). Valid until submitted, after which @data must not
 * be touched again: the slot belongs to the consumer.
 *
 * @data points into the buffer's own storage, so it stays valid across other
 * reservations and pops, and until ringbuf_mpsc_destroy().
 *
 * Distinct from ringbuf_zcopy.h's struct ringbuf_rsv, and deliberately not
 * interchangeable with it: the handles name tickets in different buffers.
 */
struct ringbuf_mpsc_rsv {
    void *data;
    size_t ticket;
};

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
 * ringbuf_mpsc_reserve - claim a slot to fill in place
 * @rb: the buffer
 * @rsv: handle to fill in, valid only on a true return
 *
 * Callable from any number of threads concurrently, with NO caller-held lock
 * -- which is the difference from ringbuf_zcopy_reserve(). Never blocks.
 *
 * On success @rsv->data points at slot_size writable bytes that are yours
 * until you submit; nothing is delivered to the consumer until
 * ringbuf_mpsc_submit() is called on this exact handle, which you MUST do
 * exactly once (see the note at the top of this header -- there is no
 * cancel).
 *
 * Any number of reservations may be outstanding at once, up to the buffer's
 * capacity, across all producers, and they may be submitted in any order.
 *
 * A false return has three causes, none of them an error:
 *
 *   - The buffer is full of unread messages. Lossless mode only; in
 *     overwrite mode this evicts instead.
 *   - The slot is still held by an earlier reservation that has not been
 *     submitted yet. This happens in BOTH modes -- an unsubmitted slot is
 *     never evicted, since a filler may be writing to it. It is about the
 *     reservation's age rather than how many are outstanding: the slot
 *     wanted here was reserved exactly capacity tickets ago. With several
 *     producers the blocking reservation is usually a peer's, so retrying is
 *     productive.
 *   - Overwrite mode only: the consumer is mid-read of the exact slot being
 *     evicted. Transient.
 *
 * A false return never consumes a ticket, so retrying is always safe.
 *
 * Return: true if a slot was claimed and @rsv filled in; false for any of
 * the three reasons above.
 */
bool ringbuf_mpsc_reserve(struct ringbuf_mpsc *rb,
                          struct ringbuf_mpsc_rsv *rsv);

/**
 * ringbuf_mpsc_submit - publish a filled reservation to the consumer
 * @rb: the buffer
 * @rsv: handle from a successful ringbuf_mpsc_reserve()
 *
 * Callable from ANY thread -- in particular the one that filled the slot,
 * which is what lets several fills proceed in parallel. Never fails, never
 * blocks, and takes no lock in either mode. Concurrent submits of distinct
 * reservations are safe; they touch disjoint slots.
 *
 * Exactly once per reservation. Submitting a handle twice, or one that
 * reserve() did not fill in, is undefined behavior.
 *
 * After this returns, @rsv->data must not be read or written again.
 *
 * The message becomes visible to the consumer in RESERVATION order, not
 * submit order: an earlier reservation that is still unsubmitted keeps this
 * one waiting, and until it arrives ringbuf_mpsc_pop() reports false even
 * though the buffer is not empty.
 */
void ringbuf_mpsc_submit(struct ringbuf_mpsc *rb,
                         const struct ringbuf_mpsc_rsv *rsv);

/**
 * ringbuf_mpsc_push - copy one message into the buffer
 * @rb: the buffer
 * @data: slot_size bytes to copy in
 *
 * Callable from any number of threads concurrently. Never blocks. Exactly
 * reserve + memcpy + submit, for callers that already have the message
 * assembled elsewhere and gain nothing from filling in place; it leaves no
 * reservation outstanding on return.
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
 * empty, in either mode. The oldest ticket may be claimed but not yet
 * published, which blocks delivery head-of-line: later messages can already
 * be sitting in the ring, but handing them out early would reorder the
 * stream. Two ways to get there -- a producer mid-push, or a reservation
 * that has not been submitted yet -- and the second can last arbitrarily
 * long, since it clears only when the caller submits.
 *
 * This is inherent to multiple producers and to the split push, and it is
 * why the consumer must also retry rather than treating false as "drained".
 * A consumer may conclude the stream is over only once every producer has
 * finished AND every reservation has been submitted. Use
 * ringbuf_mpsc_is_empty_approx() if you need to distinguish the two, bearing
 * in mind that it is approximate.
 *
 * Return: true if a message was copied out; false if the buffer is empty, if
 * the oldest message is still mid-publish or awaiting its submit, or -- in
 * overwrite mode -- on transient contention with an evicting producer.
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
 * concurrently. Counts tickets claimed, so a message still mid-publish, or a
 * reservation not yet submitted, makes this report non-empty while
 * ringbuf_mpsc_pop() still returns false.
 */
bool ringbuf_mpsc_is_empty_approx(const struct ringbuf_mpsc *rb);

/**
 * ringbuf_mpsc_is_full_approx - diagnostic fullness check
 * @rb: the buffer
 *
 * Return: true if the buffer looked full. The result may already be stale
 * by the time it is read, since producers and the consumer can run
 * concurrently. Counts tickets claimed, so outstanding reservations count as
 * occupying their slots -- which they do.
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
