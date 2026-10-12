#ifndef RINGBUF_ZCOPY_H
#define RINGBUF_ZCOPY_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Ring buffer with a split push: reserve a slot, fill it in place, then submit
 * it. Single-producer / single-consumer by default, and multi-producer when
 * the caller serializes the claims -- see "SEVERAL PRODUCERS" below.
 *
 * Every SPSC variant in this tree copies the payload inside push(), from a
 * buffer the caller already filled. This one hands the caller a pointer to
 * the slot itself and lets it write there directly:
 *
 *	struct ringbuf_rsv a, b, c;
 *	ringbuf_zcopy_reserve(rb, &a);
 *	ringbuf_zcopy_reserve(rb, &b);
 *	ringbuf_zcopy_reserve(rb, &c);
 *
 *	fill(a.data);		-- these three may run CONCURRENTLY, on
 *	fill(b.data);		-- other threads, in a DMA engine, as an
 *	fill(c.data);		-- in-flight read(2), ...
 *
 *	ringbuf_zcopy_submit(rb, &c);	-- in ANY order
 *	ringbuf_zcopy_submit(rb, &a);
 *	ringbuf_zcopy_submit(rb, &b);
 *
 * Two things that buys: the fill writes straight into the slot, with no
 * staging buffer, and several fills can overlap each other instead of being
 * serialized by one thread's memcpy. Messages are delivered in RESERVATION
 * order regardless of submit order -- submitting c first does not deliver it
 * first.
 *
 * The contract, which is SPSC like ringbuf.h and ringbuf_mutex.h:
 *
 *   - Exactly ONE thread may call ringbuf_zcopy_reserve() (or the push()
 *     convenience wrapper). More than one is undefined behavior -- UNLESS the
 *     caller serializes them, which is a supported arrangement in its own
 *     right; see "SEVERAL PRODUCERS" below.
 *   - Exactly ONE thread may call ringbuf_zcopy_pop(). More than one is
 *     undefined behavior, and no caller-held lock fixes it: pop advances the
 *     read index with a plain single-writer store, so two poppers would
 *     deliver the same message twice.
 *   - ringbuf_zcopy_submit() may be called from ANY thread, which is the
 *     point: it is what the filler threads call when they finish.
 *
 * SEVERAL PRODUCERS. Any number of threads may reserve, provided EVERY call to
 * reserve() (and push()) is serialized by one mutex the CALLER supplies.
 * Nothing in this file synchronizes producers against each other; what makes
 * the arrangement work is that reserve's internals need the claims
 * SERIALIZED, not same-threaded.
 *
 *	pthread_mutex_lock(&my_push_mu);		-- serialize the CLAIM
 *	bool got = ringbuf_zcopy_reserve(rb, &rsv);
 *	pthread_mutex_unlock(&my_push_mu);
 *
 *	if (got) {
 *		fill(rsv.data);			-- OUTSIDE the lock, so
 *		ringbuf_zcopy_submit(rb, &rsv);	   every producer's fill
 *	}					   overlaps every other's
 *
 * That is worth doing only because the lock covers so little. Claiming a slot
 * is a few atomics on fixed state; filling it is a memcpy, a read(2), a
 * serialization pass -- unbounded, and the part that costs. A mutex around an
 * ordinary push() necessarily covers the payload copy too, which serializes
 * exactly the work you wanted to overlap.
 *
 * Three rules come with it:
 *
 *   1. Hold the mutex across reserve() ONLY. Holding it across the fill is
 *      correct but pointless -- that is a serialized push with extra steps.
 *   2. NEVER require it on the submit path. A producer spinning on reserve()
 *      is usually blocked by a PEER's unsubmitted slot, so if that peer needs
 *      the same mutex to submit, the two deadlock. Retry with the mutex
 *      released.
 *   3. It must be a real lock. reserve() reads and writes the internal write
 *      index with relaxed atomics, so one producer's store reaches the next
 *      producer's load only through the mutex's own release/acquire edges. Any
 *      pthread_mutex_t or correctly built spinlock supplies them; a "lock"
 *      assembled from relaxed atomics does not, and would compile and mostly
 *      appear to work.
 *
 * Delivery is then by RESERVATION order -- the order the mutex granted claims
 * in -- so two producers racing for it may be delivered in either order, while
 * each producer's own messages stay in the order it reserved them.
 *
 * EVERY SUCCESSFUL RESERVATION MUST BE SUBMITTED, exactly once. There is no
 * cancel. Abandoning a reservation stalls delivery permanently: the consumer
 * waits for that slot forever and the buffer wedges once the producer laps
 * the ring. On an error path, submit a sentinel payload the consumer can
 * recognize -- do not simply drop the handle.
 *
 * BOTH SIDES MUST RETRY. pop() can return false while the buffer is not
 * empty, because a reserved-but-unsubmitted slot blocks delivery
 * head-of-line; reserve() can return false for the mirror-image reason. See
 * the notes on the two functions. A caller that treats false as terminal
 * will stall.
 *
 * Relation to the other variants. This is ringbuf_mutex.h with push() split
 * in two: same data structure, same two modes, and overwrite mode is still
 * reconciled with one internal mutex, so reserve()/push() may BLOCK and
 * neither is async-signal-safe. Use ringbuf.h if that matters.
 *
 * ringbuf_mpsc.h is the other way to take several producers, and the closest
 * variant to this one: it offers the SAME reserve/submit split, so the payload
 * is written in place there too. The difference is purely who synchronizes the
 * producers. It does so itself with a ticket CAS, needing no caller-held mutex
 * and staying lock-free and async-signal-safe; this file needs the caller's
 * mutex but keeps its write index single-writer and its internals simpler.
 *
 * Pick ringbuf_mpsc.h when producers must not block, you cannot hold a lock,
 * or you have several producers and a small or heavily-contended ring. Pick
 * this one for a genuine SPSC producer that wants in-place fills -- where no
 * mutex is needed at all -- or for several producers on a roomy ring with
 * cores to spare. BENCHMARK_ringbuf_mpsc_vs_ringbuf_zcopy.md measures the
 * two; the result is a split decision that turns on contention rather than on
 * payload size or fill cost.
 *
 * The one structural difference follows from out-of-order submit. In
 * ringbuf_mutex.h the write index doubles as the readiness marker: everything
 * below it is stored, everything at or above it is not. That cannot work here
 * -- reservations are handed out in order but completed in any order, so
 * "stored" is no longer a prefix of the ring. Readiness therefore moves to a
 * per-slot flag, one per slot, which is what submit sets and pop tests.
 *
 * Used only through ringbuf_zcopy_push(), there are never any outstanding
 * reservations, and the variant is contract-identical to ringbuf_mutex.h --
 * including overwrite mode's promise that push never fails.
 *
 * THE LAYOUT IS VISIBLE. struct ringbuf_zcopy is defined below rather than kept
 * opaque, so a caller can read a field directly, size the allocation itself, or
 * inline a hot path. That is a deliberate exception: ringbuf.h,
 * ringbuf_mutex.h and ringbuf_vslot.h keep theirs private. The fields remain
 * INTERNAL -- read them if it helps, but every WRITE belongs to the functions
 * here, which is where the memory ordering each field documents is actually
 * established. In particular the internal mutex is NOT the push mutex the
 * SEVERAL PRODUCERS rules ask for: it is unused in lossless mode and not even
 * initialized there, so supply your own. Nothing below changes behavior for a
 * caller who ignores it.
 */

/**
 * struct ringbuf_zcopy - the ring buffer
 * @capacity: slot count, a power of two >= 2
 * @mask: @capacity - 1, for mapping a ticket to a slot
 * @slot_size: bytes per slot, as passed to ringbuf_zcopy_create()
 * @overwrite_on_full: the mode chosen at create time
 * @stamp: per-slot publication state, @capacity entries, pointing into @tail
 * @dropped_count: messages evicted before they were read
 * @write_idx: next ticket to hand out; single-writer, see below
 * @lock: reconciles the two writers of @read_idx; overwrite mode only
 * @read_idx: next ticket to deliver
 * @tail: slot data, then the stamp array
 *
 * Defined here rather than kept opaque so callers can see the layout:
 * read a field directly, size the allocation themselves, or inline a hot
 * path. The fields are nonetheless INTERNAL -- read them if you like, but
 * every write belongs to the functions below, which is where the ordering
 * documented per field is established. Use ringbuf_zcopy_capacity() and
 * friends in portable code; they are not going to change.
 *
 * @tail is a flexible array member, so this struct CANNOT be a member of
 * another one (C11 6.7.2.1p3) -- deriving from it means wrapping the
 * pointer, or allocating the block yourself and treating the result as one
 * of these. In the latter case ringbuf_zcopy_create() is the specification
 * of what a valid block looks like: the tail needs @capacity * @slot_size
 * bytes, the stamp array aligned for atomic_size_t after them, and
 * stamp[i] = i.
 */
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

    /* Single-writer: only reserve() ever stores to write_idx, in either mode,
     * exactly as in ringbuf_mutex.c. The split push does not change that --
     * submit() touches only the stamp, never this counter.
     *
     * Note what "single-writer" rests on: reserve() being serialized, not
     * single-threaded. With several producers under the caller's push mutex
     * there is still only one store in flight, which is why this stays a
     * plain counter rather than becoming a CAS target like ringbuf_mpsc.c's.
     *
     * Relaxed throughout, which is the one ordering that got WEAKER than
     * ringbuf_mutex.c's. There the release store on write_idx is what
     * publishes the message; here publication is the stamp's job, and nothing
     * reads write_idx to decide a slot is ready: only reserve itself, the
     * locked eviction path, and the two advisory *_approx helpers.
     *
     * The relaxed load/store pair is also the one place the MULTI-producer
     * arrangement depends on the caller rather than on this file: producer B's
     * load must observe producer A's store, and relaxed atomics alone promise
     * no such thing -- the caller's unlock/lock supplies the edge. Hence the
     * rule above that the mutex be a real lock. Deliberately not promoted
     * to acquire/release: that would duplicate a mandatory mutex on every
     * reserve, buying nothing, while implying this counter could carry
     * concurrent producers on its own, which it cannot. */
    _Alignas(64) atomic_size_t write_idx;

    /* Retirement counter, and the one place the clean SPSC split breaks: in
     * overwrite mode the producer advances it too, when it evicts, so it has
     * two writers.
     *
     * `lock` is what reconciles them, held by every pop and by reserve's
     * eviction branch only -- the arrangement from ringbuf_mutex.c, kept
     * unchanged because there are still just TWO kinds of writer, not N+1 as
     * in ringbuf_mpsc.c, however many producers the caller runs: only one of
     * them can be inside the eviction branch at a time. In particular read_idx
     * needs no CAS -- a plain store under this lock is already single-valued --
     * whereas ringbuf_mpsc.c needs advance_read_idx()'s store-max loop
     * precisely because it declined to serialize its producers. Unused, and
     * not even initialized, in lossless mode.
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
 * struct ringbuf_rsv - a claimed slot, awaiting submit
 * @data: slot_size writable bytes; fill these before submitting
 * @ticket: internal identifier of the claimed slot; do not interpret it
 *
 * Caller-allocated, and by value -- stack-allocate one per reservation,
 * rather than asking the buffer for one. Filled in by ringbuf_zcopy_reserve() and consumed by
 * ringbuf_zcopy_submit(). Valid until submitted, after which @data must not
 * be touched again: the slot belongs to the consumer.
 *
 * @data points into the buffer's own storage, so it stays valid across other
 * reservations and pops, and until ringbuf_zcopy_destroy().
 */
struct ringbuf_rsv {
    void *data;
    size_t ticket;
};

/**
 * ringbuf_zcopy_create - allocate a ring buffer
 * @slot_size: size of each slot in bytes; must be non-zero
 * @min_capacity: lower bound on the slot count, rounded up to the next
 *	power of two >= @min_capacity (minimum 2)
 * @overwrite_on_full: if true, a reserve on a full buffer evicts the oldest
 *	unread slot (counted in ringbuf_zcopy_dropped_count()) instead of
 *	failing. If false, a reserve on a full buffer fails.
 *
 * Return: the new buffer, or NULL on allocation failure, mutex-init
 * failure, or invalid arguments.
 */
struct ringbuf_zcopy *ringbuf_zcopy_create(size_t slot_size,
                                           size_t min_capacity,
                                           bool overwrite_on_full);

/**
 * ringbuf_zcopy_destroy - free a ring buffer
 * @rb: buffer from ringbuf_zcopy_create(), or NULL
 *
 * No producer or consumer may be running concurrently, and no reservation
 * may still be outstanding -- @rb owns the memory every handle points into.
 */
void ringbuf_zcopy_destroy(struct ringbuf_zcopy *rb);

/**
 * ringbuf_zcopy_reserve - claim a slot to fill in place
 * @rb: the buffer
 * @rsv: handle to fill in, valid only on a true return
 *
 * Producer side only, and single-caller. On success @rsv->data points at
 * slot_size writable bytes that are yours until you submit; nothing is
 * delivered to the consumer until ringbuf_zcopy_submit() is called on this
 * exact handle, which you MUST do exactly once (see the note at the top of
 * this header -- there is no cancel).
 *
 * Any number of reservations may be outstanding at once, up to the
 * buffer's capacity, and they may be submitted in any order.
 *
 * A false return has two causes, and neither is an error:
 *
 *   - The buffer is full of unread messages. Lossless mode only; in
 *     overwrite mode this evicts instead, and may block briefly on the
 *     consumer rather than failing.
 *   - The slot is still held by an earlier reservation of yours that has
 *     not been submitted yet. This happens in BOTH modes -- an unsubmitted
 *     slot is never evicted, since a filler may be writing to it. It is
 *     about the reservation's age rather than how many are outstanding:
 *     the slot wanted here was reserved exactly capacity tickets ago. With one
 *     producer only your own submit can clear it, so retrying without
 *     submitting something will spin forever; with several, the blocking
 *     reservation is usually a peer's, so retrying -- with the push mutex
 *     released -- is the correct response.
 *
 * Return: true if a slot was claimed and @rsv filled in; false if the
 * buffer is full (lossless mode) or the slot it needs is reserved but
 * unsubmitted (either mode).
 */
bool ringbuf_zcopy_reserve(struct ringbuf_zcopy *rb, struct ringbuf_rsv *rsv);

/**
 * ringbuf_zcopy_submit - publish a filled reservation to the consumer
 * @rb: the buffer
 * @rsv: handle from a successful ringbuf_zcopy_reserve()
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
 * one waiting, and until it arrives ringbuf_zcopy_pop() reports false even
 * though the buffer is not empty.
 */
void ringbuf_zcopy_submit(struct ringbuf_zcopy *rb,
                          const struct ringbuf_rsv *rsv);

/**
 * ringbuf_zcopy_push - reserve, copy one message in, and submit
 * @rb: the buffer
 * @data: slot_size bytes to copy in
 *
 * Producer side only. Convenience wrapper for callers that already have the
 * message assembled elsewhere and gain nothing from filling in place; it
 * leaves no reservation outstanding on return.
 *
 * Used exclusively, this makes the buffer behave exactly like
 * ringbuf_mutex.h: since there is never an unsubmitted slot, the second
 * cause of a false return from reserve() cannot arise, so in overwrite mode
 * this ALWAYS returns true.
 *
 * Return: true if the message was stored. In lossless mode, false if the
 * buffer is full. In overwrite mode, always true: it evicts the oldest
 * unread slot and counts it in ringbuf_zcopy_dropped_count(), blocking
 * briefly if the consumer is mid-read of that exact slot.
 */
bool ringbuf_zcopy_push(struct ringbuf_zcopy *rb, const void *data);

/**
 * ringbuf_zcopy_pop - copy one message out of the buffer
 * @rb: the buffer
 * @data: destination for slot_size bytes
 *
 * Consumer side only, and single-caller. On a false return the contents of
 * @data are unspecified and must not be used.
 *
 * A false return does NOT imply the buffer is empty, in either mode. The
 * oldest slot may be reserved but not yet submitted, which blocks delivery
 * head-of-line: later messages can already be sitting in the ring, but
 * handing them out early would reorder the stream. This is inherent to the
 * split push, and it is why the consumer must retry rather than treating
 * false as "drained". Use ringbuf_zcopy_is_empty_approx() if you need to
 * distinguish the two, bearing in mind that it is approximate.
 *
 * Return: true if a message was copied out; false if the buffer is empty or
 * the oldest slot is still awaiting its submit.
 */
bool ringbuf_zcopy_pop(struct ringbuf_zcopy *rb, void *data);

/**
 * ringbuf_zcopy_capacity - slot count of the buffer
 * @rb: the buffer
 *
 * Return: the capacity chosen at create time; it never changes.
 */
size_t ringbuf_zcopy_capacity(const struct ringbuf_zcopy *rb);

/**
 * ringbuf_zcopy_slot_size - size of one slot
 * @rb: the buffer
 *
 * Return: the slot size in bytes, as passed to ringbuf_zcopy_create(). This
 * is how many bytes a reservation's @data points at.
 */
size_t ringbuf_zcopy_slot_size(const struct ringbuf_zcopy *rb);

/**
 * ringbuf_zcopy_dropped_count - messages evicted before they were read
 * @rb: the buffer
 *
 * Return: the number of messages reserve() has evicted. Always 0 if the
 * buffer was created with overwrite_on_full = false.
 */
size_t ringbuf_zcopy_dropped_count(const struct ringbuf_zcopy *rb);

/**
 * ringbuf_zcopy_is_empty_approx - diagnostic emptiness check
 * @rb: the buffer
 *
 * Return: true if the buffer looked empty. The result may already be stale
 * by the time it is read, since the other side can concurrently
 * reserve/submit/pop. Counts reservations, so a slot still awaiting its
 * submit makes this report non-empty while ringbuf_zcopy_pop() still
 * returns false.
 */
bool ringbuf_zcopy_is_empty_approx(const struct ringbuf_zcopy *rb);

/**
 * ringbuf_zcopy_is_full_approx - diagnostic fullness check
 * @rb: the buffer
 *
 * Return: true if the buffer looked full. The result may already be stale
 * by the time it is read, since the other side can concurrently
 * reserve/submit/pop. Counts reservations, so outstanding ones count as
 * occupying their slots -- which they do.
 */
bool ringbuf_zcopy_is_full_approx(const struct ringbuf_zcopy *rb);

#ifdef RINGBUF_TEST_HOOKS
/**
 * ringbuf_zcopy_test_seed_counters - reposition the counters, tests only
 * @rb: the buffer, which must be empty
 * @start: value to park both internal counters at
 *
 * Compiled in only under -DRINGBUF_TEST_HOOKS. Leaves the buffer empty.
 * This exists so tests can exercise the point where the counters overflow
 * SIZE_MAX, which would otherwise require 2^64 reservations to reach.
 *
 * Also reinitializes the per-slot stamps, which hold absolute ticket
 * numbers and would otherwise desynchronize from the seeded counters.
 *
 * Preconditions: the buffer must be empty with no reservation outstanding,
 * and no producer or consumer may be running concurrently.
 */
void ringbuf_zcopy_test_seed_counters(struct ringbuf_zcopy *rb, size_t start);
#endif

#endif /* RINGBUF_ZCOPY_H */
