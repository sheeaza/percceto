#define _GNU_SOURCE

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ringbuf_vslot.h"

/* Bytes of length header in front of every message body. */
#define HDR_SIZE 4u

/* Records are aligned to this so the uint32_t header is never a torn
 * access, including when a record straddles the physical end of the ring
 * (the mirror makes that span contiguous, but not necessarily aligned
 * unless every record start is). */
#define REC_ALIGN 4u

struct ringbuf_vslot {
    size_t capacity; /* whole-ring size in bytes; a power of two */
    size_t mask; /* capacity - 1 */
    size_t max_msg_size; /* per-message ceiling; longer bodies truncated */
    bool overwrite_on_full;

    /* Start of the mirrored mapping: capacity usable bytes, followed by a
     * second view of those same bytes. Both halves are one mapping of
     * 2 * capacity, so this is also what munmap takes.
     *
     * The mirror is what makes a record that runs past the physical end
     * contiguous in virtual memory. Writing at base + (counter & mask)
     * therefore needs no second mask, no split memcpy, and no branch: the
     * furthest byte any record touches is (capacity - 1) + reclen - 1,
     * and reclen <= capacity is guaranteed at create time, so the span
     * always lands inside the 2 * capacity mapping. */
    char *base;

    /* Eviction and truncation accounting. Relaxed throughout: these are
     * diagnostics and order nothing. */
    atomic_size_t dropped_count;
    atomic_size_t truncated_count;

    /* Single-writer: only the producer ever stores to write_idx, in either
     * mode, which is why it keeps a cheap relaxed-load / release-store
     * pair rather than a CAS. A BYTE count, not a message count --
     * advancing by each record's padded length is what makes the records
     * dense. */
    _Alignas(64) atomic_size_t write_idx;
    size_t cached_read; /* producer-private cache of read_idx */

    /* Retirement counter, and the one place the clean SPSC split breaks.
     *
     * In lossless mode the consumer is its only writer and a blind release
     * store suffices. In overwrite mode the producer writes it too, when
     * it evicts, so read_idx has two writers.
     *
     * Unlike ringbuf.c those two are NOT reconciled with a CAS, because
     * boundary_lock below is held across the whole of both operations, so
     * a plain store is already single-valued. received + dropped ==
     * pushed still holds: every advance happens under the lock and moves
     * read_idx by exactly one record's length, so each record is retired
     * exactly once, as a drop or as a receive.
     *
     * The cache-line padding is still worth it: evictions only happen when
     * the buffer is full, so the common case leaves this line to the
     * consumer alone.
     *
     * boundary_lock shares the line deliberately -- an eviction takes it
     * and then immediately stores read_idx, so pairing them costs one line
     * transfer instead of two, and the line stays split from write_idx,
     * which the producer owns. */
    _Alignas(64) atomic_size_t read_idx;

    /* Guards the single record where an in-flight pop can overlap with an
     * evicting push. ringbuf.c needs a whole per-slot array; here one lock
     * suffices, because both sides only ever touch the record AT read_idx
     * -- the consumer to take it, the producer to evict it -- so there is
     * exactly one contested record at any moment.
     *
     * A spinlock rather than a mutex, so push stays non-blocking and
     * signal-safe: whichever side fails to acquire returns false, which is
     * a transient contention signal and not an error. Both push and pop
     * can therefore fail spuriously in overwrite mode and every caller
     * must retry -- the same contract as ringbuf.c. 0 = free, 1 = held.
     *
     * Only used in overwrite mode; lossless mode has a single writer to
     * read_idx and never touches it. */
    atomic_int boundary_lock;

    /* Consumer-private cache of write_idx. Plain size_t, deliberately: the
     * producer never touches it. This is also what makes ringbuf_vslot_pop
     * non-re-entrant -- two concurrent poppers would race here even with
     * read_idx fully synchronized. */
    size_t cached_write;
};

/**
 * add_checked - add two sizes, detecting overflow
 * @a: first addend
 * @b: second addend
 *
 * Return: @a + @b, or 0 on overflow. 0 is never a legitimate total at any
 * call site here, so callers treat it as failure.
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
 * next_pow2 - round up to a power of two
 * @v: value to round
 *
 * Return: the smallest power of two >= @v, or 0 if that would overflow.
 * Unlike ringbuf.c's version there is no floor of 2: callers here have
 * already raised @v to at least a page.
 */
static size_t next_pow2(size_t v)
{
    size_t p = 1;
    while (p < v) {
        if (p > SIZE_MAX / 2)
            return 0;
        p <<= 1;
    }
    return p;
}

/**
 * record_len - total ring footprint of a message body
 * @size: message length in bytes
 *
 * Return: the header plus @size, padded to REC_ALIGN, or 0 on overflow.
 */
static size_t record_len(size_t size)
{
    size_t with_hdr = add_checked(HDR_SIZE, size);
    if (with_hdr == 0)
        return 0;
    return align_up(with_hdr, REC_ALIGN);
}

/**
 * map_mirrored - map @bytes of memory twice, back to back
 * @bytes: size of the ring; must be a non-zero multiple of the page size
 *
 * Reserves 2 * @bytes of address space with PROT_NONE and then maps the
 * same memfd over both halves, so that [base, base + bytes) and
 * [base + bytes, base + 2 * bytes) are two views of one set of physical
 * pages. A record running off the end of the first view reappears at the
 * start of it, which is what lets the hot paths copy every record with a
 * single memcpy and no wrap branch.
 *
 * The reserve-then-overmap order matters: two independent mmap calls can
 * land anywhere, and the halves must be adjacent. The fd is closed before
 * returning -- the mappings keep the object alive.
 *
 * NOTE for anyone reading a clean ThreadSanitizer run as proof this file
 * is race-free: tsan keys its shadow memory by virtual address, so the
 * same physical byte seen at base + o and at base + o + bytes looks like
 * two unrelated locations to it. That can only HIDE a race on a record
 * which straddles the end, never invent one -- so tsan is a weaker gate
 * here than it is for ringbuf.c and ringbuf_mutex.c. The straddle and
 * wrap_* cases in test_ringbuf_vslot.c, which check sequence numbers and
 * lengths rather than relying on instrumentation, are the real gate.
 *
 * Return: the base of the mapping, or NULL on failure.
 */
static char *map_mirrored(size_t bytes)
{
    int fd = memfd_create("ringbuf_vslot", MFD_CLOEXEC);
    if (fd < 0)
        return NULL;

    char *base = NULL;

    if (ftruncate(fd, (off_t)bytes) != 0)
        goto out;

    /* Reserve the whole span first so the two views are guaranteed
     * adjacent; MAP_FIXED below then overwrites this placeholder. */
    void *reserved =
        mmap(NULL, bytes * 2, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reserved == MAP_FAILED)
        goto out;

    for (size_t half = 0; half < 2; half++) {
        void *want = (char *)reserved + half * bytes;
        void *got = mmap(want, bytes, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_FIXED, fd, 0);
        if (got == MAP_FAILED) {
            /* Drops the placeholder and whichever view already
             * succeeded, in one call. */
            munmap(reserved, bytes * 2);
            goto out;
        }
    }

    base = reserved;

out:
    close(fd);
    return base;
}

/**
 * ringbuf_vslot_create - allocate a variable-length ring buffer
 * @max_msg_size: per-message ceiling; longer bodies are truncated to it
 * @min_ring_bytes: whole-buffer floor for all queued messages together
 * @overwrite_on_full: evict the oldest messages instead of failing
 *
 * See ringbuf_vslot.h for the caller-visible contract, including what the
 * two sizes are for and how far @min_ring_bytes gets rounded up. Unlike
 * ringbuf.c there are two allocations, not one: the control block, and the
 * mirrored mapping of the ring. The flexible-array-member trick the other
 * variants use cannot work here, because the ring has to be a separate
 * mapping to be mirrored at all.
 *
 * Return: the new buffer, or NULL on allocation failure, mapping failure,
 * or invalid arguments.
 */
struct ringbuf_vslot *ringbuf_vslot_create(size_t max_msg_size,
                                           size_t min_ring_bytes,
                                           bool overwrite_on_full)
{
    if (max_msg_size == 0)
        return NULL;

    /* Every step is overflow-checked because max_msg_size and min_ring_bytes are
     * caller-supplied; an unchecked capacity could wrap and under-map the
     * ring, turning every later record write into an out-of-bounds one. */
    size_t one_record = record_len(max_msg_size);
    if (one_record == 0)
        return NULL;

    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0)
        return NULL;
    size_t capacity = (size_t)page;

    /* Raise to whatever binds hardest: the caller's request, a page (mmap
     * granularity), or one max_msg_size record. That last one is what
     * guarantees reclen <= capacity for every record, which is the
     * premise the single-memcpy mirror arithmetic rests on. */
    if (min_ring_bytes > capacity)
        capacity = min_ring_bytes;
    if (one_record > capacity)
        capacity = one_record;

    capacity = next_pow2(capacity);
    if (capacity == 0)
        return NULL;
    /* The mapping is twice the ring. */
    if (capacity > SIZE_MAX / 2)
        return NULL;
    /* next_pow2 of something >= page is a multiple of the page size, since
     * the page size is itself a power of two; map_mirrored requires it. */

    struct ringbuf_vslot *rb = NULL;
    if (posix_memalign((void **)&rb, _Alignof(struct ringbuf_vslot),
                       sizeof(*rb)) != 0)
        return NULL;

    rb->base = map_mirrored(capacity);
    if (!rb->base) {
        free(rb);
        return NULL;
    }

    rb->capacity = capacity;
    rb->mask = capacity - 1;
    rb->max_msg_size = max_msg_size;
    rb->overwrite_on_full = overwrite_on_full;
    atomic_init(&rb->dropped_count, (size_t)0);
    atomic_init(&rb->truncated_count, (size_t)0);
    atomic_init(&rb->write_idx, (size_t)0);
    rb->cached_read = 0;
    atomic_init(&rb->read_idx, (size_t)0);
    atomic_init(&rb->boundary_lock, 0);
    rb->cached_write = 0;

    return rb;
}

/**
 * ringbuf_vslot_destroy - free a ring buffer
 * @rb: buffer from ringbuf_vslot_create(), or NULL
 *
 * Two steps, unlike the single free() the other variants manage: the
 * mirrored mapping, then the control block.
 */
void ringbuf_vslot_destroy(struct ringbuf_vslot *rb)
{
    if (!rb)
        return;
    /* One munmap covers both views: they were carved out of a single
     * 2 * capacity reservation. */
    munmap(rb->base, rb->capacity * 2);
    free(rb);
}

/**
 * read_record_len - length of the message stored at a ring offset
 * @rb: the buffer
 * @idx: byte counter positioned at a record header
 *
 * Return: the stored body length, read from the record's header.
 */
static size_t read_record_len(const struct ringbuf_vslot *rb, size_t idx)
{
    uint32_t len;
    memcpy(&len, rb->base + (idx & rb->mask), HDR_SIZE);
    return len;
}

/**
 * store_record - write header and body at a ring offset
 * @rb: the buffer
 * @idx: byte counter to write the record at
 * @data: @size bytes to copy in, or NULL when @size is 0
 * @size: body length, already clamped to max_msg_size
 *
 * One memcpy each for header and body, with no wrap handling: the mirror
 * makes the span contiguous even when it runs off the physical end (see
 * map_mirrored).
 */
static void store_record(struct ringbuf_vslot *rb, size_t idx, const void *data,
                         size_t size)
{
    size_t off = idx & rb->mask;
    uint32_t len = (uint32_t)size;

    memcpy(rb->base + off, &len, HDR_SIZE);
    if (size)
        memcpy(rb->base + off + HDR_SIZE, data, size);
}

/**
 * ringbuf_vslot_push - copy one message into the buffer
 * @rb: the buffer
 * @data: @size bytes to copy in, or NULL when @size is 0
 * @size: body length; anything over max_msg_size is truncated
 *
 * Producer side only. See ringbuf_vslot.h for the caller-visible contract.
 * The eviction branch is the only path that takes boundary_lock, and the
 * only one that can fail transiently.
 *
 * Return: true if the message was stored; false if it does not fit
 * (lossless mode), or on transient lock contention with an in-flight pop
 * (overwrite mode).
 */
bool ringbuf_vslot_push(struct ringbuf_vslot *rb, const void *data, size_t size)
{
    if (size > rb->max_msg_size) {
        size = rb->max_msg_size;
        atomic_fetch_add_explicit(&rb->truncated_count, 1,
                                  memory_order_relaxed);
    }

    size_t reclen = record_len(size);
    size_t write_idx =
        atomic_load_explicit(&rb->write_idx, memory_order_relaxed);
    size_t next = write_idx + reclen;

    if (next - rb->cached_read > rb->capacity) {
        /* Acquire, and it is doing real work: it pairs with pop's release
         * store so the consumer's finished read is visible before we
         * overwrite that space. The lock below cannot substitute, because
         * this load runs before we take it and the fast path never takes
         * it at all. */
        rb->cached_read =
            atomic_load_explicit(&rb->read_idx, memory_order_acquire);
        if (next - rb->cached_read > rb->capacity) {
            if (!rb->overwrite_on_full)
                return false; /* genuinely no room */

            /* No room, so the space we need starts at the oldest unread
             * record -- exactly the one a pop may be reading. Take the
             * boundary lock for the whole eviction. */
            int expected = 0;
            if (!atomic_compare_exchange_strong_explicit(
                    &rb->boundary_lock, &expected, 1, memory_order_acquire,
                    memory_order_relaxed))
                return false; /* consumer is mid-read of the oldest record */

            /* Relaxed: in overwrite mode pop touches read_idx only under
             * this same lock, so the unlock/lock edge already orders us
             * against it. Same reasoning for the store at the end of the
             * loop and for write_idx below. Every non-relaxed ordering in
             * this file has an unlocked access on at least one end. */
            size_t fresh_read =
                atomic_load_explicit(&rb->read_idx, memory_order_relaxed);

            /* Evict until the message fits. Variable sizes mean one push
             * can need to retire several records -- the loop is the main
             * structural difference from the fixed-size variants, where
             * one push evicts at most one slot. */
            size_t dropped = 0;
            while (next - fresh_read > rb->capacity) {
                fresh_read += record_len(read_record_len(rb, fresh_read));
                dropped++;
            }

            if (dropped) {
                atomic_store_explicit(&rb->read_idx, fresh_read,
                                      memory_order_relaxed);
                atomic_fetch_add_explicit(&rb->dropped_count, dropped,
                                          memory_order_relaxed);
            }
            rb->cached_read = fresh_read;

            store_record(rb, write_idx, data, size);
            atomic_store_explicit(&rb->write_idx, next, memory_order_relaxed);
            atomic_store_explicit(&rb->boundary_lock, 0, memory_order_release);
            return true;
        }
    }

    /* There is room, so this record cannot overlap the one an in-flight
     * pop is reading -- hence no lock on the hot path. The release store
     * publishes the record to the consumer. */
    store_record(rb, write_idx, data, size);
    atomic_store_explicit(&rb->write_idx, next, memory_order_release);
    return true;
}

/**
 * ringbuf_vslot_pop - copy one message out of the buffer
 * @rb: the buffer
 * @data: destination, or NULL when @cap is 0
 * @cap: bytes available at @data
 * @len: out-parameter for the stored length, which may exceed @cap
 *
 * Consumer side only. See ringbuf_vslot.h for the caller-visible contract.
 * In overwrite mode the whole operation runs under boundary_lock.
 *
 * Return: true if a message was copied out; false if the buffer is empty,
 * or on transient contention with an evicting producer (overwrite mode).
 */
bool ringbuf_vslot_pop(struct ringbuf_vslot *rb, void *data, size_t cap,
                       size_t *len)
{
    if (!rb->overwrite_on_full) {
        /* Lossless mode: read_idx has exactly one writer (us), so this is
         * the plain lock-free path. */
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

        size_t size = read_record_len(rb, read_idx);
        size_t copy = size < cap ? size : cap;
        if (copy)
            memcpy(data, rb->base + ((read_idx & rb->mask) + HDR_SIZE), copy);
        *len = size;

        atomic_store_explicit(&rb->read_idx, read_idx + record_len(size),
                              memory_order_release);
        return true;
    }

    /* Overwrite mode: hold the lock across the whole operation, so an
     * evicting producer cannot touch this record mid-copy. */
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&rb->boundary_lock, &expected,
                                                 1, memory_order_acquire,
                                                 memory_order_relaxed))
        return false; /* producer is evicting this record right now */

    /* read_idx is re-read HERE, under the lock, never reused from before
     * it. This is load-bearing: while we were acquiring, the producer may
     * have evicted the very message we were about to take, counted it in
     * dropped_count, and refilled the space. A stale read_idx would then
     * have us read the HEADER of that newer record and hand it out as the
     * evicted one -- and a bogus length would drive the memcpy below. */
    size_t read_idx = atomic_load_explicit(&rb->read_idx, memory_order_relaxed);
    if ((ptrdiff_t)(rb->cached_write - read_idx) <= 0) {
        rb->cached_write =
            atomic_load_explicit(&rb->write_idx, memory_order_acquire);
        if ((ptrdiff_t)(rb->cached_write - read_idx) <= 0) {
            atomic_store_explicit(&rb->boundary_lock, 0, memory_order_release);
            return false; /* genuinely empty */
        }
    }

    size_t size = read_record_len(rb, read_idx);
    size_t copy = size < cap ? size : cap;
    if (copy)
        memcpy(data, rb->base + ((read_idx & rb->mask) + HDR_SIZE), copy);
    *len = size;

    /* Release, and the one store under this lock that is not relaxed:
     * push's fast path reads read_idx without ever taking the lock, so the
     * unlock's release does not reach it. This store is what tells that
     * path our read is complete and the space is safe to reuse. */
    atomic_store_explicit(&rb->read_idx, read_idx + record_len(size),
                          memory_order_release);

    atomic_store_explicit(&rb->boundary_lock, 0, memory_order_release);
    return true;
}

/**
 * ringbuf_vslot_capacity_bytes - whole-buffer storage, in bytes
 * @rb: the buffer
 *
 * Return: the capacity in bytes, as chosen at create time.
 */
size_t ringbuf_vslot_capacity_bytes(const struct ringbuf_vslot *rb)
{
    return rb->capacity;
}

/**
 * ringbuf_vslot_max_msg_size - per-message ceiling, in bytes
 * @rb: the buffer
 *
 * Return: max_msg_size as passed to ringbuf_vslot_create().
 */
size_t ringbuf_vslot_max_msg_size(const struct ringbuf_vslot *rb)
{
    return rb->max_msg_size;
}

/**
 * ringbuf_vslot_dropped_count - messages evicted before they were read
 * @rb: the buffer
 *
 * Return: the eviction count; always 0 in lossless mode.
 */
size_t ringbuf_vslot_dropped_count(const struct ringbuf_vslot *rb)
{
    return atomic_load_explicit(&rb->dropped_count, memory_order_relaxed);
}

/**
 * ringbuf_vslot_truncated_count - messages stored with their tail cut off
 * @rb: the buffer
 *
 * Return: the producer-side truncation count.
 */
size_t ringbuf_vslot_truncated_count(const struct ringbuf_vslot *rb)
{
    return atomic_load_explicit(&rb->truncated_count, memory_order_relaxed);
}

/**
 * ringbuf_vslot_is_empty_approx - diagnostic emptiness check
 * @rb: the buffer
 *
 * Return: true if the buffer looked empty; may be stale on return.
 */
bool ringbuf_vslot_is_empty_approx(const struct ringbuf_vslot *rb)
{
    size_t w = atomic_load_explicit(&rb->write_idx, memory_order_acquire);
    size_t r = atomic_load_explicit(&rb->read_idx, memory_order_acquire);
    return (ptrdiff_t)(w - r) <= 0;
}

/**
 * ringbuf_vslot_free_bytes_approx - diagnostic free-space check
 * @rb: the buffer
 *
 * Return: the free space in bytes; may be stale on return.
 */
size_t ringbuf_vslot_free_bytes_approx(const struct ringbuf_vslot *rb)
{
    size_t w = atomic_load_explicit(&rb->write_idx, memory_order_acquire);
    size_t r = atomic_load_explicit(&rb->read_idx, memory_order_acquire);
    ptrdiff_t used = (ptrdiff_t)(w - r);

    if (used < 0)
        used = 0; /* stale sample mid-eviction */
    if ((size_t)used > rb->capacity)
        return 0;
    return rb->capacity - (size_t)used;
}

#ifdef RINGBUF_TEST_HOOKS
/**
 * ringbuf_vslot_test_seed_counters - reposition both counters, tests only
 * @rb: the buffer, which must be empty and quiescent
 * @start: value to park both counters at, rounded down to REC_ALIGN
 *
 * Relaxed stores throughout: the preconditions rule out any concurrent
 * producer or consumer to synchronize with.
 *
 * The rounding keeps every record 4-byte aligned, which is what stops the
 * length header from being a torn access. Alignment survives the SIZE_MAX
 * overflow because 2^64 is a multiple of both REC_ALIGN and the capacity.
 */
void ringbuf_vslot_test_seed_counters(struct ringbuf_vslot *rb, size_t start)
{
    start &= ~(size_t)(REC_ALIGN - 1);

    atomic_store_explicit(&rb->write_idx, start, memory_order_relaxed);
    atomic_store_explicit(&rb->read_idx, start, memory_order_relaxed);
    rb->cached_read = start;
    rb->cached_write = start;
}
#endif
