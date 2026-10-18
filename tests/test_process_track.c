/* The simple API's thread tracks have the process track as their parent.
 *
 * pf_trace_open() registers a process track for getpid(). The first _sp call
 * on a thread creates that thread's track, and it must name the process track
 * as its parent. Reads the trace's track table directly, since the public API
 * does not expose the process uuid.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
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

/* Returns the index of the first track matching @kind and @tid, or -1. */
static int find_track(const struct pf_trace *t, enum track_kind kind,
                      int32_t tid)
{
    for (unsigned i = 0; i < t->track_count; i++) {
        const struct track_def *td = &t->tracks[i];
        if (td->kind == kind && (kind != TRACK_THREAD || td->tid == tid))
            return (int)i;
    }
    return -1;
}

int main(void)
{
    printf("test_process_track\n");

    const char *path = "process_track.pftrace";
    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = path;
    struct pf_trace *t = pf_trace_open(&cfg);
    if (!t) {
        check(0, "open trace");
        printf("1 failure(s)\n");
        return 1;
    }

    check(t->process_uuid != 0, "pf_trace_open registers a process track");

    int p = find_track(t, TRACK_PROCESS, 0);
    check(p >= 0 && t->tracks[p].uuid == t->process_uuid &&
              t->tracks[p].pid == (int32_t)getpid(),
          "process track has the uuid and pid of this process");

    /* The first _sp call on this thread registers its thread track. */
    pf_slice_begin_sp("probe");
    pf_slice_end_sp();

    int th = find_track(t, TRACK_THREAD, (int32_t)gettid());
    check(th >= 0, "first _sp call registers this thread's track");
    check(th >= 0 && t->tracks[th].parent_uuid == t->process_uuid,
          "thread track is a child of the process track");

    pf_trace_close(t);
    unlink(path);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
