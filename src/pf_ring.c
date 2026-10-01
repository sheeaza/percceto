#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pf_ring.h"

/* A record's tag lives at its start and is the only atomically accessed
 * field. The mirror guarantees the whole record is contiguous, and every
 * record is 8-byte aligned, so this cast is well defined.
 *
 * In unbounded (snapshot) mode a producer may be overwriting the very
 * record a reader is inspecting -- that is the mode's defining behaviour,
 * not a defect. The reader therefore treats everything it loads from the
 * ring as provisional: the tag's lap check (pf_ring_next) opens a seqlock
 * and pf_ring_still_valid closes it, so bytes that were being rewritten
 * are discarded rather than trusted. Loading the tag atomically is what
 * keeps that check itself well defined; ThreadSanitizer will still flag
 * the body read against the producer's plain stores, which is why
 * .tsan-suppressions exists.
 */
static _Atomic uint64_t *tag_at(const struct pf_ring *r, uint64_t pos)
{
    return (_Atomic uint64_t *)(void *)(r->base + (pos & r->mask));
}

static uint64_t tag_make(const struct pf_ring *r, uint64_t pos, uint32_t len)
{
    return ((pos >> r->log2cap) << 32) | (uint64_t)len;
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

static unsigned log2_exact(size_t v)
{
    unsigned n = 0;
    while ((((size_t)1) << n) != v)
        n++;
    return n;
}

int pf_ring_init(struct pf_ring *r, size_t want_bytes, bool bounded,
                 bool single_producer)
{
    memset(r, 0, sizeof(*r));

    if (want_bytes < PF_RING_MIN_BYTES)
        want_bytes = PF_RING_MIN_BYTES;
    size_t cap = round_up_pow2(want_bytes);

    /* The ring must hold more than one maximum-size record for the
     * bounded-mode space check to ever succeed. PF_RING_MIN_BYTES is far
     * above PF_REC_MAX, so this only guards absurd configs. */
    if (cap < 2 * PF_REC_MAX) {
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
    r->log2cap = log2_exact(cap);
    r->bounded = bounded;
    r->single_producer = single_producer;
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

uint8_t *pf_ring_claim(struct pf_ring *r, uint32_t need, uint64_t *off_out)
{
    uint64_t off;

    if (!r->bounded) {
        /* Snapshot mode: overwriting the oldest record is intended, so
         * the claim cannot fail and needs no loop. */
        if (r->single_producer) {
            off = atomic_load_explicit(&r->head, memory_order_relaxed);
            atomic_store_explicit(&r->head, off + need, memory_order_relaxed);
        } else {
            off =
                atomic_fetch_add_explicit(&r->head, need, memory_order_relaxed);
        }

        /* Keep @tail inside the window this claim leaves intact.
         *
         * Nothing stops producers from lapping it: that is the mode's
         * defining behaviour. But @tail must stay on a record boundary that
         * has not been overwritten, or the reader has no position it can
         * walk from. Checking here rather than in a caller's "am I near the
         * end" heuristic is what makes that hold -- the check sees the
         * @head this claim actually produced, so there is no window between
         * deciding and claiming for another producer to slip through.
         *
         * Trimming to half the ring, not to the brink: @tail only has to be
         * recoverable, and leaving a margin means the next few claims find
         * it already correct and skip the walk entirely. */
        uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
        if (off + need - tail > r->cap)
            pf_ring_trim(r, r->cap / 2);
    } else {
        /* Bounded mode: decide before moving @head, so that a refusal
         * leaves no gap. */
        off = atomic_load_explicit(&r->head, memory_order_relaxed);
        for (;;) {
            uint64_t tail =
                atomic_load_explicit(&r->tail, memory_order_acquire);
            if (off + need - tail > r->cap) {
                atomic_fetch_add_explicit(&r->dropped, 1, memory_order_relaxed);
                return NULL;
            }
            if (r->single_producer) {
                atomic_store_explicit(&r->head, off + need,
                                      memory_order_relaxed);
                break;
            }
            /* On failure @off is reloaded with the current head, so the
             * space check is redone against it. */
            if (atomic_compare_exchange_weak_explicit(
                    &r->head, &off, off + need, memory_order_relaxed,
                    memory_order_relaxed))
                break;
        }
    }

    *off_out = off;
    return r->base + (off & r->mask);
}

void pf_ring_publish(struct pf_ring *r, uint64_t off, uint32_t need)
{
    /* Release: everything written into the record above is visible to any
     * reader that acquire-loads this tag. */
    atomic_store_explicit(tag_at(r, off), tag_make(r, off, need),
                          memory_order_release);
    atomic_fetch_add_explicit(&r->published, 1, memory_order_relaxed);
}

void pf_ring_read_begin(struct pf_ring *r, struct pf_ring_reader *rd)
{
    rd->pos = atomic_load_explicit(&r->tail, memory_order_relaxed);
    rd->end = atomic_load_explicit(&r->head, memory_order_acquire);
}

const uint8_t *pf_ring_next(struct pf_ring *r, struct pf_ring_reader *rd,
                            uint32_t *len_out)
{
    if (rd->pos >= rd->end)
        return NULL;

    uint64_t w = atomic_load_explicit(tag_at(r, rd->pos), memory_order_acquire);
    uint32_t len = (uint32_t)w;
    uint64_t lap = w >> 32;

    /* One comparison rejects not-yet-written (zero tag), mid-write (stale
     * tag) and lapped (newer tag) records alike. */
    if (lap != (rd->pos >> r->log2cap) || len == 0)
        return NULL;
    if (len > PF_REC_MAX || (len & (PF_REC_ALIGN - 1)) != 0)
        return NULL; /* corrupt; stop rather than walk into nonsense */
    if (rd->pos + len > rd->end)
        return NULL;

    const uint8_t *p = r->base + (rd->pos & r->mask);
    rd->pos += len;
    *len_out = len;
    return p;
}

bool pf_ring_still_valid(const struct pf_ring *r, uint64_t pos)
{
    if (r->bounded)
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

bool pf_ring_trim(struct pf_ring *r, size_t retain_bytes)
{
    if (retain_bytes > r->cap)
        retain_bytes = r->cap;

    uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    if (head - tail <= retain_bytes)
        return true;

    uint64_t target = head - retain_bytes;
    uint64_t pos = tail;

    /* Walk record by record so @tail lands on a real boundary; a bare
     * seek to @target would leave it mid-record and unrecoverable.
     *
     * The walk starts at the oldest record, which is also the one most
     * likely to be recycled underneath it. That is unavoidable: record
     * lengths are only discoverable forwards, so there is no way to find a
     * boundary near @head without starting from a known one. When a
     * producer wins that race the walk resyncs, which is correct rather
     * than exceptional -- see pf_ring_resync. Trimming often enough that
     * @tail stays well clear of @head is what keeps it rare. */
    while (pos < target) {
        uint64_t w = atomic_load_explicit(tag_at(r, pos), memory_order_acquire);
        uint32_t len = (uint32_t)w;
        if ((w >> 32) != (pos >> r->log2cap) || len == 0 || len > PF_REC_MAX ||
            (len & (PF_REC_ALIGN - 1)) != 0) {
            pf_ring_resync(r);
            return false;
        }
        pos += len;
    }

    /* Only ever advance. Trims can run concurrently (a producer trimming
     * inline while the drain thread does the same), and a plain store of a
     * staler @pos would drag @tail backwards onto bytes already recycled. */
    uint64_t cur = atomic_load_explicit(&r->tail, memory_order_relaxed);
    while (cur < pos) {
        if (atomic_compare_exchange_weak_explicit(&r->tail, &cur, pos,
                                                  memory_order_release,
                                                  memory_order_relaxed))
            break;
    }
    return true;
}

void pf_ring_resync(struct pf_ring *r)
{
    uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    atomic_store_explicit(&r->tail, head, memory_order_release);
    atomic_fetch_add_explicit(&r->resyncs, 1, memory_order_relaxed);
}
