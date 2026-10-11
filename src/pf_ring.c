#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pf_ring.h"

/* A record's tag lives at its start and is the only atomically accessed
 * field. The mirror guarantees the whole record is contiguous, and every
 * record is 8-byte aligned, so this cast is well defined.
 *
 * In an overwriting (snapshot) ring a producer may be overwriting the very
 * record a reader is inspecting -- that is the mode's defining behaviour,
 * not a defect. The reader therefore treats everything it loads from the
 * ring as provisional: the tag check (pf_ring_next) opens a seqlock
 * and pf_ring_still_valid closes it, so bytes that were being rewritten
 * are discarded rather than trusted. Loading the tag atomically is what
 * keeps that check itself well defined; ThreadSanitizer will still flag
 * the body read against the producer's plain stores, which is why
 * .tsan-suppressions exists.
 */
static _Atomic uint64_t *tag_at(const struct pf_ring *r, uint64_t pos)
{
    void *at = r->base + (pos & r->mask);
    return &((struct pf_rec_hdr *)at)->tag;
}

/* The tag a record at @pos carries once published: its own end position.
 * Biased past @pos so it is never zero, which is what distinguishes a
 * published record from the zeroed pages a fresh mapping starts as. */
static uint64_t tag_make(uint64_t pos)
{
    return pos + PF_REC_BYTES;
}

static size_t round_up_pow2(size_t v)
{
    size_t p = 1;
    while (p < v) {
        size_t next = p << 1;
        if (next < p) /* overflow: give back the largest we can express */
            return p;
        p = next;
    }
    return p;
}

int pf_ring_init(struct pf_ring *r, size_t want_bytes, bool overwrite)
{
    memset(r, 0, sizeof(*r));

    if (want_bytes < PF_RING_MIN_BYTES)
        want_bytes = PF_RING_MIN_BYTES;
    size_t cap = round_up_pow2(want_bytes);

    /* The ring must hold more than one record for the dropping mode's
     * space check to ever succeed. PF_RING_MIN_BYTES is far above a single
     * record, so this only guards absurd configs. */
    if (cap < 2 * PF_REC_BYTES) {
        errno = EINVAL;
        return -1;
    }

    int fd = memfd_create("pftrace-ring", MFD_CLOEXEC);
    if (fd < 0)
        return -1;
    if (ftruncate(fd, (off_t)cap) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    /* Reserve one contiguous 2*cap window first, so the two mappings are
     * guaranteed adjacent; mapping them separately could race another
     * thread's mmap and land apart. */
    uint8_t *base = mmap(NULL, 2 * cap, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    /* Map the same pages at both halves. A write past cap lands in the
     * mirror, which is the same memory as the front of the buffer. */
    for (size_t half = 0; half < 2; half++) {
        void *want = base + half * cap;
        void *got = mmap(want, cap, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_FIXED, fd, 0);
        if (got == MAP_FAILED) {
            int saved = errno;
            munmap(base, 2 * cap);
            close(fd);
            errno = saved;
            return -1;
        }
    }

    /* The mappings hold a reference; the descriptor is no longer needed. */
    close(fd);

    r->base = base;
    r->cap = cap;
    r->mask = cap - 1;
    r->overwrite = overwrite;
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    atomic_init(&r->dropped, 0);
    atomic_init(&r->resyncs, 0);
    atomic_init(&r->published, 0);
    return 0;
}

void pf_ring_destroy(struct pf_ring *r)
{
    if (!r || !r->base)
        return;
    munmap(r->base, 2 * r->cap);
    r->base = NULL;
    r->cap = 0;
}

/* The reservation for the record at absolute @pos, with the tag submit
 * will store precomputed -- see struct pf_rec_res. */
static struct pf_rec_res res_at(const struct pf_ring *r, uint64_t pos)
{
    void *at = r->base + (pos & r->mask);
    return (struct pf_rec_res){ .rec = at, .tag = tag_make(pos) };
}

struct pf_rec_res pf_ring_reserve(struct pf_ring *r)
{
    uint64_t off;

    if (r->overwrite) {
        /* Snapshot mode: overwriting the oldest record is intended, so
         * the reservation cannot fail and needs no loop. */
        off = atomic_fetch_add_explicit(&r->head, PF_REC_BYTES,
                                        memory_order_relaxed);

        /* Keep @tail within one ring of the @head this reservation just
         * produced.
         *
         * Nothing stops producers from lapping it: that is the mode's
         * defining behaviour. But once @head - @tail exceeds @cap the
         * reader's starting point is a position whose record has already
         * been recycled, and pf_ring_next rejects it immediately -- the
         * ring would read as empty and a dump would produce nothing.
         * Checking here rather than in a caller's "am I near the end"
         * heuristic is what makes that hold: the check sees the @head this
         * reservation actually produced, so there is no window between
         * deciding and reserving for another producer to slip through.
         *
         * Note this is about the width of the window, not about landing on
         * a boundary -- with a fixed stride every position is a boundary.
         * Trimming to half the ring, not to the brink, so the next few
         * reservations find @tail already inside the window and skip the
         * call entirely. */
        uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
        if (off + PF_REC_BYTES - tail > r->cap)
            pf_ring_trim(r, r->cap / 2);

        return res_at(r, off);
    }

    /* Continuous mode: decide before moving @head, so that a refusal
     * leaves no gap. */
    off = atomic_load_explicit(&r->head, memory_order_relaxed);
    for (;;) {
        uint64_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
        if (off + PF_REC_BYTES - tail > r->cap) {
            atomic_fetch_add_explicit(&r->dropped, 1, memory_order_relaxed);
            return (struct pf_rec_res){ .rec = NULL };
        }
        /* On failure @off is reloaded with the current head, so the
         * space check is redone against it. */
        if (atomic_compare_exchange_weak_explicit(&r->head, &off,
                                                  off + PF_REC_BYTES,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed))
            return res_at(r, off);
    }
}

void pf_ring_submit(struct pf_ring *r, struct pf_rec_res res)
{
    /* Release: everything written into the record is visible to any reader
     * that acquire-loads this tag. */
    atomic_store_explicit(&res.rec->tag, res.tag, memory_order_release);
    atomic_fetch_add_explicit(&r->published, 1, memory_order_relaxed);
}

void pf_ring_read_begin(struct pf_ring *r, struct pf_ring_reader *rd)
{
    rd->pos = atomic_load_explicit(&r->tail, memory_order_relaxed);
    rd->end = atomic_load_explicit(&r->head, memory_order_acquire);
}

const uint8_t *pf_ring_next(struct pf_ring *r, struct pf_ring_reader *rd)
{
    if (rd->pos >= rd->end)
        return NULL;

    uint64_t w = atomic_load_explicit(tag_at(r, rd->pos), memory_order_acquire);

    /* One comparison rejects not-yet-written (zero tag), mid-write (stale
     * tag) and lapped (newer tag) records alike. The fixed stride is what
     * reduces it to this: there is no length to validate, and @end is a
     * whole number of records ahead, so a published record always fits. */
    if (w != tag_make(rd->pos))
        return NULL;

    const uint8_t *p = r->base + (rd->pos & r->mask);
    rd->pos += PF_REC_BYTES;
    return p;
}

bool pf_ring_still_valid(const struct pf_ring *r, uint64_t pos)
{
    if (!r->overwrite)
        return true; /* producers never overwrite unconsumed bytes */
    uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    return head - pos <= r->cap;
}

void pf_ring_read_commit(struct pf_ring *r, struct pf_ring_reader *rd)
{
    /* Release: the reader is done with these bytes before a producer can
     * see the space as free. */
    atomic_store_explicit(&r->tail, rd->pos, memory_order_release);
}

void pf_ring_trim(struct pf_ring *r, size_t retain_bytes)
{
    if (retain_bytes > r->cap)
        retain_bytes = r->cap;

    uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    if (head - tail <= retain_bytes)
        return;

    /* Round the retained span down to whole records, so @pos lands on a
     * boundary and retains at least @retain_bytes. @head is a multiple of
     * the stride -- it starts at 0 and only ever advances by one record --
     * so @pos is too. Callers pass fractions of @cap, which is a power of
     * two and so never a multiple of the stride; rounding here is what
     * reconciles the two.
     *
     * This is the whole of what a variable-length ring needed a
     * record-by-record walk for, and the walk could lose its boundary to a
     * producer recycling the record it started from. */
    uint64_t pos = head - retain_bytes / PF_REC_BYTES * PF_REC_BYTES;

    /* Only ever advance. Trims can run concurrently (a producer trimming
     * inline while the drain thread does the same), and a plain store of a
     * staler @pos would drag @tail backwards onto bytes already recycled. */
    uint64_t cur = tail;
    while (cur < pos) {
        if (atomic_compare_exchange_weak_explicit(&r->tail, &cur, pos,
                                                  memory_order_release,
                                                  memory_order_relaxed))
            break;
    }
}

void pf_ring_resync(struct pf_ring *r)
{
    uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    atomic_store_explicit(&r->tail, head, memory_order_release);
    atomic_fetch_add_explicit(&r->resyncs, 1, memory_order_relaxed);
}
