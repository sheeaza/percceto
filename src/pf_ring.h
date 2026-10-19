#ifndef PF_RING_H
#define PF_RING_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The ring interface pftrace.c is written against, and the backend
 * selection that picks an implementation of it.
 *
 * Two backends exist, and they are compared rather than ranked -- see
 * BENCHMARK_ring_backends.md. Exactly one is compiled in, chosen by a macro
 * on the command line:
 *
 *   (default)              pf_ring_mpsc.c -- wraps ringbuf_mpsc
 *   PF_RING_BACKEND_ZCOPY  pf_ring_zcopy.c -- wraps ringbuf_zcopy
 *
 * Compile time rather than a vtable of function pointers. A reservation is
 * a handful of atomics on a path that costs ~130ns end to end, so an
 * indirect call that defeated inlining would be a measurable fraction of
 * the thing being measured -- and the comparison would then be partly of
 * the dispatch mechanism rather than of the rings.
 *
 * WHAT A BACKEND MUST PROVIDE. The nine functions below, plus a definition
 * of struct pf_ring in its own header. Beyond that the contracts differ in
 * ways callers can observe, and pftrace.c is written to tolerate all of
 * them:
 *
 *   - pf_ring_reserve may fail in overwrite mode. Both ringbuf
 *     backends refuse when the slot one lap back is reserved but not yet
 *     submitted, since a filler may still be writing those bytes. Callers
 *     must treat a null reservation as a drop in either mode.
 *   - The producer may need a lock. ringbuf_zcopy requires the caller to
 *     serialize every reserve, so that backend alone cannot honour the
 *     no-lock promise in pftrace.h. See pf_ring_zcopy.c.
 */

/* The payload one event occupies, not counting whatever the backend adds
 * for its own bookkeeping.
 *
 * Backend-agnostic deliberately. The ringbuf backends keep readiness in a
 * separate per-slot array and need no in-record header at all. Folding a
 * backend's header into this constant would make every backend pay for it.
 *
 * pftrace.c pins its struct pf_rec to this with a _Static_assert. */
#define PF_PAYLOAD_BYTES 48u

/* Every payload is a multiple of 8 bytes, so each of its fields stays
 * naturally aligned and a backend may hand out 8-byte aligned slots. */
#define PF_REC_ALIGN 8u
_Static_assert(PF_PAYLOAD_BYTES % PF_REC_ALIGN == 0,
               "the payload keeps every field 8-byte aligned");

/* The smallest ring, in slots. 2048 slots is the 64 KiB floor the byte-based
 * interface had at the 32-byte payload. */
#define PF_RING_MIN_SLOTS 2048u

/* How hard pf_ring_reserve() retries a refused claim before counting a
 * drop, and how often it yields while doing so.
 *
 * SHARED BY EVERY BACKEND, and that is the point: the retry policy is a
 * property of this interface rather than of any one ring, so the
 * comparison in BENCHMARK_ring_backends.md isolates the cost of claiming
 * a slot instead of accidentally measuring two different drop policies.
 * Getting this wrong is not subtle -- an early version of the benchmark
 * gave only the slot-indexed backends a retry loop, and the mmap ring
 * then posted nearly double the throughput while dropping 33% of events
 * against their 0.05%. Both numbers were real; the comparison was
 * meaningless.
 *
 * Bounded, because a refusal can mean a peer was preempted mid-fill and
 * holds its slot for a scheduler quantum. pftrace's fill is four stores
 * with no syscall, allocation or lock, so the window is normally
 * nanoseconds and a short spin covers it; waiting out the pathological
 * case instead would block the caller's thread for far longer than
 * dropping one event costs. The drop is counted either way.
 *
 * The yield threshold is deliberately low. Past a handful of pauses the
 * holder is not merely slow but descheduled, and further spinning burns
 * the caller's quantum without helping it along. */
#define PF_RESERVE_SPINS 64u
#define PF_RESERVE_YIELD_EVERY 8u

/**
 * struct pf_rec_res - A reservation held between reserve and submit.
 * @rec: Where to write the payload.
 * @tag: Opaque to the caller; how the backend identifies the slot.
 *
 * @rec points at %PF_PAYLOAD_BYTES writable bytes, past whatever header
 * the backend keeps for itself. Fill them and submit this exact handle.
 *
 * @tag cannot live in the slot until submit, in any backend: it
 * is a ticket number the slot's own stamp is compared against. Nor can
 * submit recompute it from @rec -- a pointer into the storage yields an
 * offset, not the absolute position or ticket, and a producer delayed a
 * full lap would recover the wrong one and stamp a live record with a tag
 * that validates. Keeping it out here is what makes a lapped producer's
 * submit land as something the reader rejects.
 *
 * Two words, so it returns in registers rather than through memory.
 */
struct pf_rec_res {
    uint8_t *rec;
    uint64_t tag;
};

/**
 * struct pf_ring_reader - A walk in progress over published records.
 * @pos: Records taken so far in this walk. Only ever moves forward.
 * @end: One past the last record this walk will consider, so a walk
 *      terminates even while producers keep publishing.
 */
struct pf_ring_reader {
    uint64_t pos;
    uint64_t end;
};

/* The backend's definition of struct pf_ring, and whatever it wants
 * inlined. Included after the types above, since a backend header may use
 * them, and before the declarations below, which need the struct complete
 * for pftrace.c to embed it. */
#if defined(PF_RING_BACKEND_ZCOPY)
#include "pf_ring_zcopy.h"
#else
#include "pf_ring_mpsc.h"
#endif

/**
 * pf_ring_init() - Create the ring.
 * @r: Ring to initialize.
 * @slot_size: Bytes per slot. Must be nonzero. pftrace passes
 *      %PF_PAYLOAD_BYTES.
 * @min_cap: Requested capacity in slots, with a floor of
 *      %PF_RING_MIN_SLOTS. Backends round it up -- to a power of two
 *      number of slots in all three -- so the result is a lower bound.
 *      Read the real figure back with pf_ring_capacity_slots().
 * @overwrite: Overwrite unconsumed records rather than dropping new ones.
 *
 * Performs the ring's one allocation. Nothing on the hot path allocates
 * afterwards, in any backend.
 *
 * Return: 0 on success, -1 with errno set on failure.
 */
int pf_ring_init(struct pf_ring *r, size_t slot_size, size_t min_cap,
                 bool overwrite);

/**
 * pf_ring_destroy() - Release the ring.
 * @r: Ring to destroy. Safe on an already-destroyed or zeroed ring, and
 *      safe to call after a failed pf_ring_init().
 */
void pf_ring_destroy(struct pf_ring *r);

/**
 * pf_ring_reserve() - Reserve space for one record.
 * @r: Ring.
 * @res: Filled in on a true return; valid until submitted.
 *
 * Reserves %PF_PAYLOAD_BYTES, owned exclusively by the caller, who may
 * then fill them concurrently with other producers filling theirs. Every
 * successful reservation must be submitted exactly once; there is no
 * cancel, and abandoning one stalls the reader behind it.
 *
 * Callable from any number of threads, except on the ringbuf_zcopy
 * backend, which requires the caller to serialize these calls -- that
 * backend's wrapper does so internally, so this signature holds
 * regardless.
 *
 * Return: true if a record was reserved and @res filled in; false if the
 * record was dropped, in which case @res is left untouched and must not be
 * submitted. The ringbuf backends can refuse in either mode -- see the
 * contract notes at the top of this file.
 */
bool pf_ring_reserve(struct pf_ring *r, struct pf_rec_res *res);

/**
 * pf_ring_submit() - Make a filled record visible to the reader.
 * @r: Ring.
 * @res: Reservation from a successful pf_ring_reserve().
 *
 * Release-stores the backend's readiness marker, so a reader that
 * acquire-loads it sees the fully written payload. Never blocks and takes
 * no lock in any backend, including the one whose reserve needs one:
 * a producer refused by a peer's unsubmitted slot must be able to make
 * progress without waiting on that peer's lock.
 */
void pf_ring_submit(struct pf_ring *r, const struct pf_rec_res *res);

/**
 * pf_ring_read_begin() - Start a walk at the oldest unconsumed record.
 * @r: Ring.
 * @rd: Walk state to initialize.
 */
void pf_ring_read_begin(struct pf_ring *r, struct pf_ring_reader *rd);

/**
 * pf_ring_next() - Advance to the next published record.
 * @r: Ring.
 * @rd: Walk state.
 *
 * Stops at the first record that is not yet published, which costs at
 * most one in-flight fill of latency and resolves on the next walk. That
 * a later record may already be readable does not help: handing it over
 * early would reorder the stream.
 *
 * Return: Pointer to %PF_PAYLOAD_BYTES contiguous bytes, or %NULL at the
 * end of the published run. The storage is the backend's and is valid only
 * until the next call on @rd.
 */
const uint8_t *pf_ring_next(struct pf_ring *r, struct pf_ring_reader *rd);

/**
 * pf_ring_capacity_slots() - Slots the ring holds.
 * @r: Ring.
 *
 * The rounded-up capacity, which is what pf_ring_init()'s @min_cap was
 * a lower bound on. Callers sizing a fraction of the ring -- a watermark,
 * a retained window -- must use this rather than what they asked for.
 *
 * Each slot is PF_PAYLOAD_BYTES, and the count excludes whatever each
 * backend keeps for its own bookkeeping, so the figure means the same
 * thing everywhere: the same @min_cap buys the same slot count on every
 * backend. The ringbuf backends hold their per-slot stamps outside the
 * slot array and so allocate more than this.
 *
 * Return: The capacity in slots.
 */
size_t pf_ring_capacity_slots(const struct pf_ring *r);

/**
 * pf_ring_stats() - Read the ring's counters.
 * @r: Ring.
 * @published: Records successfully submitted.
 * @dropped: Records refused for lack of space. Counts refusals only, not
 *      records intentionally overwritten in snapshot mode: those are the
 *      mode working as intended, and &pf_stats.dropped is documented as
 *      meaning a gap in the output.
 *
 * Every pointer is required. Relaxed loads: these are counters, and no
 * caller orders anything against them.
 */
void pf_ring_stats(const struct pf_ring *r, uint64_t *published,
                   uint64_t *dropped);

#endif /* PF_RING_H */
