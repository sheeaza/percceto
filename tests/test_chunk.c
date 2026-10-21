/* Chunk pool sizing, writer limits and drop accounting, through the public API.
 *
 * - Open refuses a buffer that cannot hold two chunks, and a zero-size chunk.
 * - A thread past max_threads drops each event it emits, and counts it.
 * - A thread that exits gives its writer slot back, so a later thread can bind it.
 * - A thread that emits into two traces keeps a writer slot in each.
 * - A single producer in continuous mode loses nothing except what the
 *   counters report, and every emitted event is accounted for: committed,
 *   dropped, or still in the producer's chunk at the moment of the read.
 * - A snapshot window that wraps the pool does not count overwrites as drops.
 * - A snapshot dump includes the unfilled chunk of a live thread.
 * - Close leaves a trace open while another thread is bound to it; that
 *   thread's exit finishes it. A thread that closes its own trace finishes it. */

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "pftrace.h"

static int failures;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        if (!(cond)) {                                           \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
            failures++;                                          \
        }                                                        \
    } while (0)

/* Chunk bytes for a given slot count: header plus records. Mirrors pf_pool_init. */
#define CHUNK_BYTES(slots) (24u + (slots) * 48u)

static void setup(struct pf_config *cfg, enum pf_mode mode, const char *dir)
{
    pf_config_init(cfg);
    cfg->path = dir;
    cfg->mode = mode;
    cfg->name = mode == PF_MODE_CONTINUOUS ? "chunk.pftrace" : NULL;
    cfg->chunk_slots = 4;
    cfg->max_threads = 8;
}

static void test_open_limits(void)
{
    struct pf_config cfg;
    setup(&cfg, PF_MODE_SNAPSHOT, ".");

    /* One chunk is not enough: a writer needs a spare while its full chunk
     * is committed. */
    cfg.buffer_bytes = CHUNK_BYTES(4);
    errno = 0;
    CHECK(pf_trace_open(&cfg) == NULL && errno == EINVAL,
          "one-chunk buffer should fail with EINVAL, got errno %d", errno);

    cfg.buffer_bytes = 2 * CHUNK_BYTES(4);
    struct pf_trace *t = pf_trace_open(&cfg);
    CHECK(t != NULL, "two-chunk buffer should open");
    pf_trace_close(t);

    cfg.buffer_bytes = 2 * CHUNK_BYTES(4);
    cfg.chunk_slots = 0;
    errno = 0;
    CHECK(pf_trace_open(&cfg) == NULL && errno == EINVAL,
          "zero chunk_slots should fail with EINVAL");
    cfg.chunk_slots = 4;

    cfg.max_threads = 0;
    errno = 0;
    CHECK(pf_trace_open(&cfg) == NULL && errno == EINVAL,
          "zero max_threads should fail with EINVAL");
}

static void *emit_some(void *arg)
{
    struct pf_trace *t = arg;
    for (int i = 0; i < 10; i++)
        pf_slice_begin(t, 0, "worker");
    return NULL;
}

/* With one writer slot, the main thread takes it and a second thread cannot
 * bind. Every event that thread emits is dropped and counted. */
static void test_writer_limit(void)
{
    struct pf_config cfg;
    setup(&cfg, PF_MODE_SNAPSHOT, ".");
    cfg.buffer_bytes = 8 * CHUNK_BYTES(4);
    cfg.max_threads = 1;

    struct pf_trace *t = pf_trace_open(&cfg);
    CHECK(t != NULL, "open failed");
    if (!t) {
        failures++;
        return;
    }

    /* Binds the main thread's writer. Its one event stays in the chunk. */
    pf_slice_begin(t, 0, "main");

    pthread_t th;
    pthread_create(&th, NULL, emit_some, t);
    pthread_join(th, NULL);

    struct pf_stats s;
    pf_trace_stats(t, &s);
    CHECK(s.dropped == 10, "second thread should drop 10 events, dropped %llu",
          (unsigned long long)s.dropped);
    pf_trace_close(t);
}

/* A thread that emits into two traces holds a writer slot in each. With one
 * slot per trace, a second thread finds both taken and drops its events in
 * both traces. Had the first trace's slot been released when the thread moved
 * to the second, the second thread would bind it and drop nothing. */
struct two_traces {
    struct pf_trace *a;
    struct pf_trace *b;
};

static void *emit_both(void *arg)
{
    struct two_traces *p = arg;
    for (int i = 0; i < 5; i++) {
        pf_slice_begin(p->a, 0, "worker");
        pf_slice_begin(p->b, 0, "worker");
    }
    return NULL;
}

static void test_bind_two_traces(void)
{
    struct pf_config ca, cb;
    setup(&ca, PF_MODE_SNAPSHOT, ".");
    setup(&cb, PF_MODE_SNAPSHOT, ".");
    ca.buffer_bytes = 8 * CHUNK_BYTES(4);
    cb.buffer_bytes = 8 * CHUNK_BYTES(4);
    ca.max_threads = 1;
    cb.max_threads = 1;

    struct pf_trace *a = pf_trace_open(&ca);
    struct pf_trace *b = pf_trace_open(&cb);
    CHECK(a != NULL && b != NULL, "open failed");
    if (!a || !b) {
        failures++;
        pf_trace_close(a);
        pf_trace_close(b);
        return;
    }

    /* The main thread alternates between the traces. */
    for (int i = 0; i < 4; i++) {
        pf_slice_begin(a, 0, "main");
        pf_slice_begin(b, 0, "main");
    }

    struct two_traces p = { a, b };
    pthread_t th;
    pthread_create(&th, NULL, emit_both, &p);
    pthread_join(th, NULL);

    struct pf_stats sa, sb;
    pf_trace_stats(a, &sa);
    pf_trace_stats(b, &sb);
    CHECK(sa.events == 4 && sb.events == 4, "main thread lost events (%llu, %llu)",
          (unsigned long long)sa.events, (unsigned long long)sb.events);
    CHECK(sa.dropped == 5 && sb.dropped == 5,
          "main thread should hold its slot in both traces (dropped %llu, %llu)",
          (unsigned long long)sa.dropped, (unsigned long long)sb.dropped);
    pf_trace_close(a);
    pf_trace_close(b);
}

/* Threads come and go, one at a time, against two writer slots. Each exiting
 * thread gives its slot back, so six threads all bind and none drops an event.
 * Stats still count every event, including those from a reused slot. */
static void test_slot_reuse(void)
{
    struct pf_config cfg;
    setup(&cfg, PF_MODE_SNAPSHOT, ".");
    cfg.buffer_bytes = 32 * CHUNK_BYTES(4);
    cfg.max_threads = 2;

    struct pf_trace *t = pf_trace_open(&cfg);
    CHECK(t != NULL, "open failed");
    if (!t) {
        failures++;
        return;
    }

    for (int i = 0; i < 6; i++) {
        pthread_t th;
        pthread_create(&th, NULL, emit_some, t);
        pthread_join(th, NULL);
    }

    struct pf_stats s;
    pf_trace_stats(t, &s);
    CHECK(s.dropped == 0, "reused slots should drop nothing, dropped %llu",
          (unsigned long long)s.dropped);
    CHECK(s.events == 60, "stats should count all 60 events, counted %llu",
          (unsigned long long)s.events);
    pf_trace_close(t);
}

/* Continuous mode, one producer, a pool small enough to saturate. The
 * producer's emits split three ways: committed (events), dropped, and still in
 * its own chunk. The tail in that chunk is at most one chunk. */
static void test_accounting(void)
{
    struct pf_config cfg;
    setup(&cfg, PF_MODE_CONTINUOUS, ".");
    cfg.buffer_bytes = 4 * CHUNK_BYTES(4);
    cfg.drain_interval_ms = 1000;
    cfg.watermark_pct = 99;

    struct pf_trace *t = pf_trace_open(&cfg);
    CHECK(t != NULL, "open failed");
    if (!t) {
        failures++;
        return;
    }

    enum {
        TOTAL = 200
    };
    for (int i = 0; i < TOTAL; i++)
        pf_instant_event(t, 0, "tick");

    struct pf_stats s;
    pf_trace_stats(t, &s);
    uint64_t accounted = s.events + s.dropped;
    CHECK(accounted <= TOTAL, "accounted %llu of %d", (unsigned long long)accounted, TOTAL);
    CHECK(TOTAL - accounted <= cfg.chunk_slots,
          "%llu events unaccounted, more than one chunk (%zu)",
          (unsigned long long)(TOTAL - accounted), cfg.chunk_slots);
    pf_trace_close(t);
}

/* Snapshot mode wraps its pool many times and must not report the overwrites
 * as drops: that is the mode working as intended. */
static void test_snapshot_wrap_is_not_a_drop(void)
{
    struct pf_config cfg;
    setup(&cfg, PF_MODE_SNAPSHOT, ".");
    cfg.buffer_bytes = 4 * CHUNK_BYTES(4);

    struct pf_trace *t = pf_trace_open(&cfg);
    CHECK(t != NULL, "open failed");
    if (!t) {
        failures++;
        return;
    }

    for (int i = 0; i < 1000; i++)
        pf_instant_event(t, 0, "tick");

    struct pf_stats s;
    pf_trace_stats(t, &s);
    CHECK(s.dropped == 0, "snapshot wrap counted %llu drops", (unsigned long long)s.dropped);

    char file[512];
    CHECK(pf_trace_dump(t, "chunk_wrap", file, sizeof(file)) == 0, "dump after wrap failed");
    pf_trace_close(t);
    unlink(file);
}

struct parked {
    struct pf_trace *t;
    sem_t emitted;
    sem_t done;
};

/* Emits two events into a chunk that never fills, then waits to be released,
 * so the thread is alive with a partial chunk for the whole dump. */
static void *emit_and_park(void *arg)
{
    struct parked *p = arg;
    pf_instant_event(p->t, 0, "parked");
    pf_instant_event(p->t, 0, "parked");
    sem_post(&p->emitted);
    sem_wait(&p->done);
    return NULL;
}

static long dump_size(const char *file)
{
    struct stat st;
    return stat(file, &st) == 0 ? (long)st.st_size : -1;
}

/* A dump from one thread includes the unfilled chunk of a live thread that
 * emitted into it. The file must grow when that thread's events arrive. */
static void test_snapshot_live_tail(void)
{
    struct pf_config cfg;
    setup(&cfg, PF_MODE_SNAPSHOT, ".");
    cfg.buffer_bytes = 8 * CHUNK_BYTES(4);

    struct pf_trace *t = pf_trace_open(&cfg);
    CHECK(t != NULL, "open failed");
    if (!t) {
        failures++;
        return;
    }

    char before[512], after[512];
    CHECK(pf_trace_dump(t, "tail_before", before, sizeof(before)) == 0, "dump before failed");

    struct parked p = { .t = t };
    sem_init(&p.emitted, 0, 0);
    sem_init(&p.done, 0, 0);
    pthread_t th;
    pthread_create(&th, NULL, emit_and_park, &p);
    sem_wait(&p.emitted);

    CHECK(pf_trace_dump(t, "tail_after", after, sizeof(after)) == 0, "dump after failed");
    CHECK(dump_size(after) > dump_size(before),
          "live thread's partial chunk missing from dump (%ld vs %ld bytes)", dump_size(after),
          dump_size(before));

    sem_post(&p.done);
    pthread_join(th, NULL);
    sem_destroy(&p.emitted);
    sem_destroy(&p.done);
    pf_trace_close(t);
    unlink(before);
    unlink(after);
}

/* Continuous mode with the drain kept idle, so the file changes only when the
 * trace finishes. A thread still bound to the trace keeps it open past close:
 * nothing reaches the file until that thread exits. */
static void test_close_waits_for_live_thread(void)
{
    struct pf_config cfg;
    setup(&cfg, PF_MODE_CONTINUOUS, ".");
    cfg.name = "live_close.pftrace";
    cfg.buffer_bytes = 8 * CHUNK_BYTES(4);
    cfg.drain_interval_ms = 60000;
    cfg.watermark_pct = 99;
    unlink(cfg.name);

    struct pf_trace *t = pf_trace_open(&cfg);
    CHECK(t != NULL, "open failed");
    if (!t) {
        failures++;
        return;
    }
    long opened = dump_size(cfg.name);

    struct parked p = { .t = t };
    sem_init(&p.emitted, 0, 0);
    sem_init(&p.done, 0, 0);
    pthread_t th;
    pthread_create(&th, NULL, emit_and_park, &p);
    sem_wait(&p.emitted);

    pf_trace_close(t);
    CHECK(dump_size(cfg.name) == opened,
          "file changed at close while a thread was bound (%ld vs %ld)", dump_size(cfg.name),
          opened);

    sem_post(&p.done);
    pthread_join(th, NULL);
    CHECK(dump_size(cfg.name) > opened, "thread exit did not finish the file (%ld bytes)",
          dump_size(cfg.name));
    sem_destroy(&p.emitted);
    sem_destroy(&p.done);
    unlink(cfg.name);
}

/* The thread that closes the trace is bound to it itself. Its reference is
 * given back by close, so the file is complete when close returns. A main
 * thread never runs the exit destructor, so this is the only way out. */
static void test_close_by_bound_thread(void)
{
    struct pf_config cfg;
    setup(&cfg, PF_MODE_CONTINUOUS, ".");
    cfg.name = "self_close.pftrace";
    cfg.buffer_bytes = 8 * CHUNK_BYTES(4);
    cfg.drain_interval_ms = 60000;
    cfg.watermark_pct = 99;
    unlink(cfg.name);

    struct pf_trace *t = pf_trace_open(&cfg);
    CHECK(t != NULL, "open failed");
    if (!t) {
        failures++;
        return;
    }
    long opened = dump_size(cfg.name);

    pf_instant_event(t, 0, "self");
    pf_trace_close(t);
    CHECK(dump_size(cfg.name) > opened, "closing thread's events missing from file (%ld bytes)",
          dump_size(cfg.name));
    unlink(cfg.name);
}

int main(void)
{
    test_open_limits();
    test_writer_limit();
    test_bind_two_traces();
    test_slot_reuse();
    test_accounting();
    test_snapshot_wrap_is_not_a_drop();
    test_snapshot_live_tail();
    test_close_waits_for_live_thread();
    test_close_by_bound_thread();

    if (failures) {
        fprintf(stderr, "chunk: %d failure(s)\n", failures);
        return 1;
    }
    printf("chunk: ok\n");
    return 0;
}
