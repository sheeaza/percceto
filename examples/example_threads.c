/* Real multi-threaded producers: each worker thread registers its own track
 * with its real pid and tid, and emits slices only on that track. */
#define _GNU_SOURCE

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include "pftrace.h"

#define NUM_WORKERS 8
#define ROUNDS_PER_WORKER 30

struct shared {
    struct pf_trace *t;
    uint64_t jobs_done_track;
    atomic_int jobs_done;
};

struct worker {
    struct shared *sh;
    int id;
};

static void *worker_main(void *arg)
{
    struct worker *w = arg;
    struct shared *sh = w->sh;
    char name[16];
    snprintf(name, sizeof(name), "worker-%d", w->id);
    pf_thread_name(name);

    for (int i = 0; i < ROUNDS_PER_WORKER; i++) {
        pf_slice_begin_g("job");

        pf_slice_begin_g("load");
        usleep(300 + w->id * 100);
        pf_slice_end_g();

        pf_slice_begin_g("process");
        usleep(300);
        if (i % 3 == 0)
            pf_instant_event_g("checkpoint");
        pf_slice_end_g();

        pf_slice_end_g(); /* job */

        atomic_fetch_add(&sh->jobs_done, 1);
    }
    return NULL;
}

static int run(void)
{
    const char *name = "demo_threads.pftrace";
    unlink(name); /* O_APPEND would otherwise extend a previous run */

    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = ".";
    cfg.name = name;
    cfg.process_name = "demo_threads";
    cfg.mode = PF_MODE_CONTINUOUS;
    cfg.buffer_bytes = 256 * 1024;
    cfg.watermark_pct = 75;
    cfg.drain_interval_ms = 20;

    if (pf_trace_open_g(&cfg) != 0) {
        perror("pf_trace_open");
        return 1;
    }
    struct pf_trace *t = pf_trace_default_g();

    struct shared sh = { .t = t };
    atomic_init(&sh.jobs_done, 0);
    sh.jobs_done_track = pf_track_counter(t, 0, "jobs_done", PF_UNIT_COUNT);

    pthread_t threads[NUM_WORKERS];
    struct worker workers[NUM_WORKERS];
    int started = 0;
    for (int i = 0; i < NUM_WORKERS; i++) {
        workers[i] = (struct worker){ .sh = &sh, .id = i };
        if (pthread_create(&threads[i], NULL, worker_main, &workers[i]) != 0) {
            perror("pthread_create");
            break;
        }
        started++;
    }

    /* Sample progress from the main thread while the workers run. */
    const int total = started * ROUNDS_PER_WORKER;
    while (atomic_load(&sh.jobs_done) < total) {
        pf_counter_set_int(t, sh.jobs_done_track, atomic_load(&sh.jobs_done));
        usleep(1000);
    }
    pf_counter_set_int(t, sh.jobs_done_track, atomic_load(&sh.jobs_done));

    for (int i = 0; i < started; i++)
        pthread_join(threads[i], NULL);

    if (started < NUM_WORKERS) {
        pf_trace_close_g();
        return 1;
    }

    if (pf_trace_flush(t, NULL, NULL, 0) != 0) {
        perror("pf_trace_flush");
        pf_trace_close_g();
        return 1;
    }

    struct pf_stats s;
    pf_trace_stats(t, &s);
    printf("  threads: %d, jobs: %d\n", NUM_WORKERS, total);
    printf("  continuous: %llu events, %llu bytes written, %llu dropped\n",
           (unsigned long long)s.events, (unsigned long long)s.bytes_written,
           (unsigned long long)s.dropped);
    if (s.dropped)
        printf("  NOTE: %llu events dropped -- buffer or interval too small\n",
               (unsigned long long)s.dropped);

    pf_trace_close_g();
    printf("  wrote %s\n", name);
    return 0;
}

int main(void)
{
    puts("multi-thread (one track per real thread):");
    return run();
}
