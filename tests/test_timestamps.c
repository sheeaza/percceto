/* Writes a trace whose timestamps the importer must reproduce exactly.
 *
 * Protobuf event timestamps are deltas on a sequence-scoped incremental clock,
 * which each chunk's header ties to the trace clock. A wrong delta or a header
 * the importer rejects still parses, and only shows as wrong times after
 * import. This stubs the clock, so every time is known, and steps it back now
 * and then, as REALTIME can, which is the case that leaves the incremental
 * clock. It writes the times of every slice begin and instant next to the
 * trace, one per line, for tools/import_trace.py --expect-ts to compare with
 * what trace_processor imports.
 *
 * Several threads and small chunks, so packets cross many chunk boundaries and
 * the writers' chunks interleave in the file. */

#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "dump_dir.h"
#include "pftrace.h"
#include "pftrace_internal.h"

#define DIR "timestamps.d"
#define THREADS 4
#define ITERS 3000
/* Begins and instants per thread: a begin each for outer and inner, and an
 * instant on every seventh iteration. */
#define SLICES_PER_THREAD (ITERS * 2 + (ITERS + 6) / 7)

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

/* Each thread's clock, and the times its begins and instants were stamped
 * with. The library reads the clock once per event, before anything else. */
static __thread uint64_t fake_ns;
static __thread uint64_t *stamps;
static __thread unsigned n_stamps;
static __thread int record_next;

int clock_gettime(clockid_t clk, struct timespec *ts)
{
    (void)clk;
    fake_ns += 37;
    /* Step back about once in a hundred readings, by more than a chunk's
     * worth of events, so a new chunk can start below an earlier one. */
    if (fake_ns % 3989 < 37)
        fake_ns -= 50000;
    if (record_next) {
        stamps[n_stamps++] = fake_ns;
        record_next = 0;
    }
    ts->tv_sec = (time_t)(fake_ns / 1000000000u);
    ts->tv_nsec = (long)(fake_ns % 1000000000u);
    return 0;
}

struct worker {
    struct pf_trace *t;
    uint64_t trk;
    uint64_t stamps[SLICES_PER_THREAD];
    unsigned n;
    int id;
};

static void *work(void *p)
{
    struct worker *w = p;
    fake_ns = 500000000000000ull + (uint64_t)w->id * 1000003u;
    stamps = w->stamps;
    for (int i = 0; i < ITERS; i++) {
        record_next = 1;
        pf_slice_begin(w->t, w->trk, "outer");
        record_next = 1;
        pf_slice_begin(w->t, w->trk, "inner");
        if (i % 7 == 0) {
            record_next = 1;
            pf_instant_event(w->t, w->trk, "tick");
        }
        pf_slice_end(w->t, w->trk);
        pf_slice_end(w->t, w->trk);
        if (i % 1000 == 999)
            fake_ns += 5000000000ull; /* seconds of idle, for a wide delta */
    }
    w->n = n_stamps;
    return NULL;
}

int main(void)
{
    printf("test_timestamps\n");
    dir_reset(DIR);

    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = DIR;
    cfg.name = "timestamps.pftrace";
    cfg.mode = PF_MODE_CONTINUOUS;
    cfg.clock_id = CLOCK_REALTIME;
    cfg.chunk_bytes = 16 * PF_REC_HEAD_BYTES;
    cfg.buffer_bytes = 16u << 20;
    struct pf_trace *t = pf_trace_open(&cfg);
    CHECK(t != NULL, "open");
    if (!t)
        return 1;

    static struct worker w[THREADS];
    pthread_t th[THREADS];
    for (int i = 0; i < THREADS; i++) {
        char name[16];
        snprintf(name, sizeof(name), "w%d", i);
        w[i].t = t;
        w[i].id = i;
        w[i].trk = pf_track_thread(t, 1, i + 1, name);
        CHECK(pthread_create(&th[i], NULL, work, &w[i]) == 0, "spawn %d", i);
    }
    for (int i = 0; i < THREADS; i++)
        pthread_join(th[i], NULL);

    struct pf_stats s;
    pf_trace_stats(t, &s);
    CHECK(s.dropped == 0, "%" PRIu64 " events dropped", s.dropped);
    pf_trace_close(t);

    FILE *f = fopen(DIR "/timestamps.expected", "w");
    CHECK(f != NULL, "open expected file");
    if (f) {
        for (int i = 0; i < THREADS; i++) {
            CHECK(w[i].n == SLICES_PER_THREAD, "thread %d stamped %u, want %d", i, w[i].n,
                  SLICES_PER_THREAD);
            for (unsigned j = 0; j < w[i].n; j++)
                fprintf(f, "%" PRIu64 "\n", w[i].stamps[j]);
        }
        fclose(f);
    }

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
