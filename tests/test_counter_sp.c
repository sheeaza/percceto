/* The _sp counter calls act on the default trace and match their explicit
 * counterparts.
 *
 * Covers: pf_thread_track_sp() returns the calling thread's track and the same
 * uuid as the thread's first _sp call; pf_track_counter_sp() nests a counter
 * under that thread track and finds an existing counter by name; the counter
 * set calls accept samples without a trace argument; the calls return 0 or
 * do nothing when no default trace is open.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "../src/pftrace_internal.h"
#include "pftrace.h"

static int failures;

static void check(int ok, const char *what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures++;
}

/* Returns the index of the track with @uuid, or -1. */
static int find_uuid(const struct pf_trace *t, uint64_t uuid)
{
    for (unsigned i = 0; i < t->track_count; i++)
        if (t->tracks[i].uuid == uuid)
            return (int)i;
    return -1;
}

int main(void)
{
    printf("test_counter_sp\n");

    /* With no default trace, every _sp call returns 0 and does nothing. */
    check(pf_thread_track_sp() == 0, "no default trace: thread track is 0");
    check(pf_track_counter_sp(0, "x", PF_UNIT_COUNT) == 0,
          "no default trace: counter track is 0");
    pf_counter_set_int_sp(1, 1);
    pf_counter_set_double_sp(1, 1.0);

    const char *path = "counter_sp.pftrace";
    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = path;
    struct pf_trace *t = pf_trace_open(&cfg);
    if (!t) {
        check(0, "open trace");
        printf("1 failure(s)\n");
        return 1;
    }

    uint64_t thr = pf_thread_track_sp();
    check(thr != 0, "pf_thread_track_sp registers the calling thread");
    int ti = find_uuid(t, thr);
    check(ti >= 0 && t->tracks[ti].kind == TRACK_THREAD,
          "the returned uuid is a thread track");

    /* A first _sp slice call must hit the same cached track. */
    pf_slice_begin_sp("probe");
    pf_slice_end_sp();
    check(pf_thread_track_sp() == thr,
          "repeat calls return the same cached uuid");

    uint64_t ctr = pf_track_counter_sp(thr, "depth", PF_UNIT_COUNT);
    check(ctr != 0, "pf_track_counter_sp creates a counter under the thread");
    int ci = find_uuid(t, ctr);
    check(ci >= 0 && t->tracks[ci].kind == TRACK_COUNTER &&
              t->tracks[ci].parent_uuid == thr,
          "counter track is a child of the calling thread's track");

    check(pf_track_counter_sp(thr, "depth", PF_UNIT_COUNT) == ctr,
          "same name and parent returns the same counter");
    check(pf_track_counter_sp(thr, "depth", PF_UNIT_SIZE_BYTES) == 0,
          "unit conflict returns 0");
    check(pf_track_counter_sp(999999, "depth", PF_UNIT_COUNT) == 0,
          "a non-track parent returns 0");

    /* Each _sp sample is one event in the ring. */
    struct pf_stats before, after;
    pf_trace_stats(t, &before);
    pf_counter_set_int_sp(ctr, 7);
    pf_counter_set_double_sp(ctr, 2.5);
    pf_trace_stats(t, &after);
    check(after.events == before.events + 2,
          "both _sp samples are published to the ring");

    pf_trace_close(t);
    unlink(path);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
