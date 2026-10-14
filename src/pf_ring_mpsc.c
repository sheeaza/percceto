#include <errno.h>
#include <sched.h>
#include <string.h>

#include "pf_ring.h"
#include "pf_util.h"

/* The ringbuf_mpsc backend. See pf_ring_mpsc.h for the design and
 * pf_ring.h for the interface it implements.
 *
 * Mostly a thin adapter: ringbuf_mpsc already offers reserve/submit with
 * in-place fills and multi-producer claims, which is the shape pftrace
 * wants. Three things need real work -- the retry policy on reserve, the
 * reader's pop-and-count, and keeping occupancy cheap -- and they are what
 * the rest of this file is.
 */

int pf_ring_init(struct pf_ring *r, size_t want_bytes, bool overwrite)
{
    memset(r, 0, sizeof(*r));

    if (want_bytes < PF_RING_MIN_BYTES)
        want_bytes = PF_RING_MIN_BYTES;

    /* Slots, not bytes: the library's unit. It rounds the count up to a
     * power of two, so the capacity read back below is >= what the
     * division asked for. */
    size_t slots = want_bytes / PF_PAYLOAD_BYTES;

    r->rb = ringbuf_mpsc_create(PF_PAYLOAD_BYTES, slots, overwrite);
    if (!r->rb) {
        /* create() reports allocation failure and invalid arguments
         * alike, without distinguishing them. slot_size is a nonzero
         * constant and the capacity is floored well above the minimum, so
         * what is left is the allocation. */
        errno = ENOMEM;
        return -1;
    }

    r->cap_bytes = ringbuf_mpsc_capacity(r->rb) * PF_PAYLOAD_BYTES;
    r->overwrite = overwrite;
    atomic_init(&r->published, 0);
    atomic_init(&r->dropped, 0);
    atomic_init(&r->consumed, 0);
    atomic_init(&r->retries, 0);
    return 0;
}

void pf_ring_destroy(struct pf_ring *r)
{
    if (!r || !r->rb)
        return;
    ringbuf_mpsc_destroy(r->rb);
    r->rb = NULL;
    r->cap_bytes = 0;
}

struct pf_rec_res pf_ring_reserve(struct pf_ring *r)
{
    struct ringbuf_mpsc_rsv rsv;

    /* BOTH SIDES MUST RETRY, per ringbuf_mpsc.h: a false return here is
     * usually transient and says nothing about the ring being full. Three
     * causes, and only the first is terminal in lossless mode:
     *
     *   - genuinely full of unread messages (lossless mode only);
     *   - the slot one lap back is reserved but unsubmitted, so a peer is
     *     mid-fill -- clears when that peer submits;
     *   - overwrite mode only: the consumer is mid-read of the exact slot
     *     being evicted -- clears when the pop finishes.
     *
     * A refusal consumes no ticket, so retrying leaves no hole. */
    for (unsigned spin = 0; spin < PF_RESERVE_SPINS; spin++) {
        if (ringbuf_mpsc_reserve(r->rb, &rsv)) {
            /* Publication is submit's job; count it there, so @published
             * never leads what a reader could see. */
            return (struct pf_rec_res){ .rec = rsv.data,
                                        .tag = (uint64_t)rsv.ticket };
        }

        atomic_fetch_add_explicit(&r->retries, 1, memory_order_relaxed);

        if (spin % PF_RESERVE_YIELD_EVERY == PF_RESERVE_YIELD_EVERY - 1)
            sched_yield();
        else
            cpu_relax();
    }

    atomic_fetch_add_explicit(&r->dropped, 1, memory_order_relaxed);
    return (struct pf_rec_res){ .rec = NULL };
}

void pf_ring_submit(struct pf_ring *r, struct pf_rec_res res)
{
    /* The ticket identifies the slot; the library's release store on its
     * stamp is what publishes the fill. Reconstructed rather than carried
     * as a struct, since pf_rec_res is two words by design. */
    struct ringbuf_mpsc_rsv rsv = { .data = res.rec,
                                    .ticket = (size_t)res.tag };
    ringbuf_mpsc_submit(r->rb, &rsv);
    atomic_fetch_add_explicit(&r->published, 1, memory_order_relaxed);
}

void pf_ring_read_begin(struct pf_ring *r, struct pf_ring_reader *rd)
{
    /* The cursor counts records, not bytes: this backend's reader pops in
     * delivery order and has no byte positions to track. @pos is how many
     * records this walk has taken, and @end bounds it so a walk
     * terminates even while producers keep publishing.
     *
     * Bounded by what was outstanding when the walk began. Relaxed is
     * enough: pop's own acquire on the slot stamp is what orders a
     * record's bytes, and a producer that publishes mid-walk is simply
     * left for the next one. */
    uint64_t pub = atomic_load_explicit(&r->published, memory_order_relaxed);
    uint64_t con = atomic_load_explicit(&r->consumed, memory_order_relaxed);
    rd->pos = 0;
    rd->end = pub - con;
}

const uint8_t *pf_ring_next(struct pf_ring *r, struct pf_ring_reader *rd)
{
    if (rd->pos >= rd->end)
        return NULL;

    /* Retires the record into our own buffer, so what we hand back is a
     * private copy nothing can reach.\n     *
     * A false return does not mean empty -- the oldest slot may be
     * reserved and still filling, which blocks delivery head-of-line.
     * Stop the walk rather than spin: those records are not lost, and the
     * next drain takes them. Spinning here would stall the drain thread on
     * a producer's fill for no gain. */
    if (!ringbuf_mpsc_pop(r->rb, r->pop_buf))
        return NULL;

    /* Only count what was actually handed over, so occupancy and the next
     * walk's bound stay honest. */
    atomic_fetch_add_explicit(&r->consumed, 1, memory_order_relaxed);
    rd->pos++;
    return r->pop_buf;
}

void pf_ring_read_commit(struct pf_ring *r, struct pf_ring_reader *rd)
{
    (void)r;
    (void)rd;
    /* Nothing deferred to release: ringbuf_mpsc_pop frees each slot as it
     * retires it, so the space was handed back during the walk rather
     * than at the end of it. */
}

size_t pf_ring_capacity_bytes(const struct pf_ring *r)
{
    return r->cap_bytes;
}

void pf_ring_stats(const struct pf_ring *r, uint64_t *published,
                   uint64_t *dropped)
{
    /* Casting away const to read atomics: these are counters, and reading
     * them does not modify the ring in any observable way. */
    struct pf_ring *m = (struct pf_ring *)r;
    *published = atomic_load_explicit(&m->published, memory_order_relaxed);
    *dropped = atomic_load_explicit(&m->dropped, memory_order_relaxed);
}
