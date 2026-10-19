/* The ring holds at least min_ring_bytes.
 *
 * Checks the slot count pftrace asks the ring for (pf_min_slots) at the
 * boundary where a floor division would be one slot short: a minimum just
 * above a whole number of slots. Then builds a ring from that count and checks
 * the capacity it actually got. */

#include <stdio.h>

#include "../src/pftrace_internal.h"

static int failures;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fputc('\n', stderr);                                 \
            failures++;                                          \
        }                                                        \
    } while (0)

/* A whole number of slots above the ring minimum, so the boundary is in
 * range of a small ring. */
#define SLOTS 4096u

static void check_slot_count(void)
{
    CHECK(pf_min_slots(0) == 0, "pf_min_slots(0) = %zu", pf_min_slots(0));
    CHECK(pf_min_slots(1) == 1, "pf_min_slots(1) = %zu", pf_min_slots(1));
    CHECK(pf_min_slots(PF_PAYLOAD_BYTES) == 1, "one slot's bytes");
    CHECK(pf_min_slots(PF_PAYLOAD_BYTES + 1) == 2, "one slot's bytes plus 1");
    CHECK(pf_min_slots((size_t)SLOTS * PF_PAYLOAD_BYTES) == SLOTS,
          "exact multiple gives exactly that many slots");
    CHECK(pf_min_slots((size_t)SLOTS * PF_PAYLOAD_BYTES + 1) == SLOTS + 1,
          "one byte over a whole number of slots needs one more slot");
}

/* Builds a ring from the slot count for @min_bytes and checks it holds at
 * least @min_bytes. Under a floor division this fails: SLOTS slots is one
 * byte short, and SLOTS is already a power of two, so the library does not
 * round it up. */
static void check_ring_holds(size_t min_bytes)
{
    struct pf_ring r;
    int rc = pf_ring_init(&r, PF_PAYLOAD_BYTES, pf_min_slots(min_bytes), false);
    CHECK(rc == 0, "ring init failed for %zu bytes", min_bytes);
    if (rc != 0)
        return;
    size_t got = pf_ring_capacity_slots(&r) * PF_PAYLOAD_BYTES;
    CHECK(got >= min_bytes, "asked for %zu bytes, ring holds %zu", min_bytes,
          got);
    pf_ring_destroy(&r);
}

int main(void)
{
    check_slot_count();
    check_ring_holds((size_t)SLOTS * PF_PAYLOAD_BYTES);
    check_ring_holds((size_t)SLOTS * PF_PAYLOAD_BYTES + 1);
    check_ring_holds((size_t)SLOTS * PF_PAYLOAD_BYTES + PF_PAYLOAD_BYTES - 1);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("min_ring: ok\n");
    return 0;
}
