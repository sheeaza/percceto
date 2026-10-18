#include <errno.h>
#include <sched.h>
#include <string.h>

#include "pf_ring.h"
#include "pf_util.h"

/* The ringbuf_zcopy backend. See pf_ring_zcopy.h for the design and
 * pf_ring.h for the interface it implements.
 *
 * Identical to pf_ring_mpsc.c except for the push mutex, which is the
 * whole substance of the comparison between them. Read that file first;
 * the retry policy, the reader's pop-and-count
 * are the same there and documented more fully.
 */

int pf_ring_init(struct pf_ring *r, size_t slot_size, size_t min_cap,
                 bool overwrite)
{
    memset(r, 0, sizeof(*r));

    if (slot_size == 0) {
        errno = EINVAL;
        return -1;
    }
    if (min_cap < PF_RING_MIN_SLOTS)
        min_cap = PF_RING_MIN_SLOTS;

    r->rb = ringbuf_zcopy_create(slot_size, min_cap,
                                 overwrite ? RINGBUF_DROP_OLDEST :
                                             RINGBUF_DROP_NEWEST);
    if (!r->rb) {
        errno = ENOMEM;
        return -1;
    }

    if (pthread_mutex_init(&r->push_lock, NULL) != 0) {
        /* Unwind, so a failed init leaves nothing allocated -- pftrace's
         * open path goes straight to its own cleanup on -1. */
        ringbuf_zcopy_destroy(r->rb);
        r->rb = NULL;
        errno = ENOMEM;
        return -1;
    }

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
    pthread_mutex_destroy(&r->push_lock);
    ringbuf_zcopy_destroy(r->rb);
    r->rb = NULL;
}

bool pf_ring_reserve(struct pf_ring *r, struct pf_rec_res *res)
{
    struct ringbuf_rsv rsv;

    for (unsigned spin = 0; spin < PF_RESERVE_SPINS; spin++) {
        /* The lock covers the CLAIM and nothing else.
         *
         * Everything this backend is for lives outside it: the caller
         * fills the slot after we return and submits without taking it at
         * all. Widening it to cover the fill would be correct and
         * pointless -- a serialized push with extra steps, which is the
         * arrangement ringbuf_zcopy exists to avoid. */
        pthread_mutex_lock(&r->push_lock);
        bool got = ringbuf_zcopy_reserve(r->rb, &rsv);
        pthread_mutex_unlock(&r->push_lock);

        if (got) {
            *res = (struct pf_rec_res){ .rec = rsv.data,
                                        .tag = (uint64_t)rsv.ticket };
            return true;
        }

        /* Retrying with the lock RELEASED, which is required rather than
         * tidy. A refusal usually means the slot one lap back is held by a
         * peer's reservation that has not been submitted yet -- and that
         * peer is filling it right now, outside the lock, about to submit
         * without it. Spinning while holding the mutex would not block
         * that peer's submit, but it would block every OTHER producer's
         * claim behind a wait that only the peer can end, converting a
         * transient refusal into a convoy. Worse, if submit had ever been
         * made to need this lock the two would deadlock outright, which is
         * why ringbuf_zcopy.h forbids that. */
        atomic_fetch_add_explicit(&r->retries, 1, memory_order_relaxed);

        if (spin % PF_RESERVE_YIELD_EVERY == PF_RESERVE_YIELD_EVERY - 1)
            sched_yield();
        else
            cpu_relax();
    }

    atomic_fetch_add_explicit(&r->dropped, 1, memory_order_relaxed);
    return false;
}

void pf_ring_submit(struct pf_ring *r, const struct pf_rec_res *res)
{
    /* No lock, deliberately: see the retry comment in pf_ring_reserve.
     * submit touches only the slot's own stamp, so concurrent submits of
     * distinct reservations are safe, and a producer blocked on reserve
     * needs this one to complete without waiting for the mutex. */
    struct ringbuf_rsv rsv = { .data = res->rec, .ticket = (size_t)res->tag };
    ringbuf_zcopy_submit(r->rb, &rsv);
    atomic_fetch_add_explicit(&r->published, 1, memory_order_relaxed);
}

void pf_ring_read_begin(struct pf_ring *r, struct pf_ring_reader *rd)
{
    uint64_t pub = atomic_load_explicit(&r->published, memory_order_relaxed);
    uint64_t con = atomic_load_explicit(&r->consumed, memory_order_relaxed);
    rd->pos = 0;
    rd->end = pub - con;
}

const uint8_t *pf_ring_next(struct pf_ring *r, struct pf_ring_reader *rd)
{
    if (rd->pos >= rd->end)
        return NULL;

    /* Never takes push_lock. That mutex serializes producers against each
     * other and says nothing about the consumer; taking it here would
     * stall every producer for the duration of a drain, which is the
     * opposite of what it is for. ringbuf_zcopy.h says so explicitly.
     *
     * A false return does not mean empty -- the oldest slot may be
     * reserved and still filling, blocking delivery head-of-line. Stop
     * the walk; the next drain takes those records. */
    if (!ringbuf_zcopy_pop(r->rb, r->pop_buf))
        return NULL;

    atomic_fetch_add_explicit(&r->consumed, 1, memory_order_relaxed);
    rd->pos++;
    return r->pop_buf;
}

size_t pf_ring_capacity_slots(const struct pf_ring *r)
{
    return ringbuf_zcopy_capacity(r->rb);
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
