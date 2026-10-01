#include <stdio.h>
#include <unistd.h>

#include "pftrace.h"

int main(void)
{
    const char *path = "demo.pftrace";
    struct pf_trace *t = pf_trace_open(path);
    if (!t) {
        fprintf(stderr, "failed to open %s for writing\n", path);
        return 1;
    }

    uint64_t proc = pf_track_process(t, 1234, "demo_app");
    uint64_t main_thread = pf_track_thread(t, proc, 1234, 1, "main");
    uint64_t worker_thread = pf_track_thread(t, proc, 1234, 2, "worker");
    uint64_t queue_depth =
        pf_track_counter(t, proc, "queue_depth", PF_UNIT_COUNT);

    for (int i = 0; i < 3; i++) {
        pf_slice_begin(t, main_thread, "compute", "app");
        pf_counter_set_int(t, queue_depth, 10 - i * 3);
        usleep(500);

        pf_slice_begin(t, main_thread, "parse", "app");
        usleep(500);
        pf_slice_end(t, main_thread); /* parse */

        pf_slice_begin(t, main_thread, "render", "app");
        usleep(500);
        pf_slice_end(t, main_thread); /* render */

        pf_slice_end(t, main_thread); /* compute */
    }

    for (int i = 0; i < 3; i++) {
        pf_slice_begin(t, worker_thread, "work_item", "worker");
        pf_counter_set_double(t, queue_depth, 4.5 + i);
        if (i % 2 == 0)
            pf_instant_event(t, worker_thread, "cache_miss", "worker");
        usleep(500);
        pf_slice_end(t, worker_thread);
    }

    pf_trace_close(t);
    printf("wrote %s\n", path);
    return 0;
}
