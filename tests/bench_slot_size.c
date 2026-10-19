/* Single claim of a 64-byte slot versus a 32-byte slot.
 *
 * Three rows, each one producer thread against the vendored ringbuf_mpsc
 * ring, which is the same backend the default build uses:
 *
 *   noargs_slot32         one 32-byte claim per event: the no-arg case
 *   args2_slot64_1claim   one 64-byte claim per event: the args case as
 *                         proposed, a single slot big enough for head plus args
 *   args2_slot32_2claims  two 32-byte claims per event: the args case as the
 *                         ring works today, for reference
 *
 * Each row fills every byte it claims (memset of the slot), so the cost
 * measured is the claim, the fill, and the drain's copy-out of those bytes.
 * The fill is not the real encoder's field stores; it isolates the cost of
 * the bytes themselves. Events are produced and drained in batches, so the
 * ring never refuses a claim.
 *
 * Output is one "name value unit" line per row.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ringbuf_mpsc.h"

#define N_EVENTS 4000000u
#define BATCH 1024u /* events per produce-then-drain round */

static uint64_t real_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* Produces and drains @events events, in batches. Each event makes
 * @slots_per_event claims of @slot bytes. */
static void cycle(struct ringbuf_mpsc *rb, uint8_t *out, size_t slot,
                  unsigned slots_per_event, uint64_t events)
{
    for (uint64_t done = 0; done < events; done += BATCH) {
        for (unsigned e = 0; e < BATCH; e++) {
            for (unsigned c = 0; c < slots_per_event; c++) {
                struct ringbuf_mpsc_rsv rsv;
                if (!ringbuf_mpsc_reserve(rb, &rsv)) {
                    fprintf(stderr, "reserve refused; batch too large\n");
                    exit(1);
                }
                memset(rsv.data, (int)(done + e), slot);
                ringbuf_mpsc_submit(rb, &rsv);
            }
        }
        for (unsigned e = 0; e < BATCH * slots_per_event; e++) {
            if (!ringbuf_mpsc_pop(rb, out)) {
                fprintf(stderr, "pop found nothing\n");
                exit(1);
            }
        }
    }
}

/* Returns ns per event. The ring is cycled through once before timing, so
 * its pages are already touched and the timed loop measures the steady
 * state, not first-touch page faults. */
static double run(size_t slot, unsigned slots_per_event, size_t cap)
{
    struct ringbuf_mpsc *rb =
        ringbuf_mpsc_create(slot, cap, RINGBUF_DROP_NEWEST);
    if (!rb) {
        perror("create");
        exit(1);
    }
    uint8_t *out = malloc(slot);
    if (!out) {
        perror("malloc");
        exit(1);
    }

    cycle(rb, out, slot, slots_per_event, cap);

    uint64_t t0 = real_ns();
    cycle(rb, out, slot, slots_per_event, N_EVENTS);
    double ns = (double)(real_ns() - t0) / (double)N_EVENTS;

    free(out);
    ringbuf_mpsc_destroy(rb);
    return ns;
}

/* Ring sizes. A small ring is reused within the cache; a large one is
 * cycled through, so its writes go to RAM. Both hold far more than one
 * batch. */
#define CAP_SMALL 4096u
#define CAP_LARGE (1u << 20)

int main(void)
{
    printf("noargs_slot32_cap4k %.1f ns/event\n", run(32, 1, CAP_SMALL));
    printf("args2_slot64_1claim_cap4k %.1f ns/event\n", run(64, 1, CAP_SMALL));
    printf("args2_slot32_2claims_cap4k %.1f ns/event\n", run(32, 2, CAP_SMALL));

    printf("noargs_slot32_cap1m %.1f ns/event\n", run(32, 1, CAP_LARGE));
    printf("args2_slot64_1claim_cap1m %.1f ns/event\n", run(64, 1, CAP_LARGE));
    printf("args2_slot32_2claims_cap1m %.1f ns/event\n", run(32, 2, CAP_LARGE));
    return 0;
}
