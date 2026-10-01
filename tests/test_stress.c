/* End-to-end stress for the two modes, driving enough events through a
 * deliberately small ring that it wraps many times over.
 *
 * The example only exercises the happy path: its event count fits the ring,
 * so nothing is ever overwritten and the interesting behaviour never
 * happens. The claims worth testing are the ones that only appear under
 * wrap pressure:
 *
 *   snapshot   -- keeps a bounded window and the window is the RECENT one,
 *                 so a dump taken after a stall contains the events just
 *                 before it, not the start of the program;
 *   continuous -- loses nothing even though the ring turns over repeatedly,
 *                 because the drain thread keeps up.
 */

#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pftrace.h"

static int failures;

#define CHECK(cond, ...)         \
    do {                         \
        if (!(cond)) {           \
            printf("  FAIL: ");  \
            printf(__VA_ARGS__); \
            printf("\n");        \
            failures++;          \
        }                        \
    } while (0)

#define RING_BYTES (64 * 1024)
#define EVENTS 200000

struct worker_arg {
    struct pf_trace *t;
    uint64_t track;
    unsigned n;
};

static void *worker_fn(void *p)
{
    struct worker_arg *a = p;
    for (unsigned i = 0; i < a->n; i++) {
        pf_slice_begin(a->t, a->track, "work", "stress");
        pf_counter_set_int(a->t, a->track, i);
        pf_slice_end(a->t, a->track);
    }
    return NULL;
}

/* Snapshot mode under wrap pressure. The ring holds far less than the
 * event stream, so the dump must contain a recent window -- and must
 * contain the marker emitted immediately before the dump. */
static void test_snapshot_window(void)
{
    printf("snapshot window (%d events through a %dKiB ring):\n", EVENTS,
           RING_BYTES / 1024);

    const char *path = "stress_snapshot.pftrace";
    unlink(path);

    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = path;
    cfg.mode = PF_MODE_SNAPSHOT;
    cfg.ring_bytes = RING_BYTES;
    cfg.drain_interval_ms = 5;

    struct pf_trace *t = pf_trace_open(&cfg);
    CHECK(t != NULL, "open");
    if (!t)
        return;

    uint64_t proc = pf_track_process(t, 4321, "stress");
    uint64_t track = pf_track_thread(t, proc, 4321, 1, "main");

    /* "early" appears only at the very start, so if it survives to the
     * dump the ring is not actually recycling. */
    pf_instant_event(t, track, "early_marker", "stress");

    for (unsigned i = 0; i < EVENTS; i++) {
        pf_slice_begin(t, track, "compute", "stress");
        pf_slice_end(t, track);
    }

    /* The stall the program just noticed. */
    pf_instant_event(t, track, "perf_drop_detected", "stress");
    CHECK(pf_trace_dump(t) == 0, "dump");

    struct pf_stats s;
    pf_trace_stats(t, &s);
    printf("  events=%" PRIu64 " written=%" PRIu64 " dropped=%" PRIu64
           " resyncs=%" PRIu64 "\n",
           s.events, s.bytes_written, s.dropped, s.resyncs);

    CHECK(s.dropped == 0, "snapshot mode never drops (it overwrites)");
    CHECK(s.events == EVENTS * 2 + 2, "every event was published");
    /* A resync means the trim thread lost its place and discarded the
     * buffered window. With the wake threshold above the retain point it
     * should keep up even against a tight producer loop; a nonzero count
     * here means a dump could come back empty. */
    CHECK(s.resyncs == 0, "trim keeps up without resyncing (%" PRIu64 ")",
          s.resyncs);

    /* The whole point: the file is a bounded window, not the full stream. */
    CHECK(s.bytes_written > 0, "dump produced output");
    CHECK(s.bytes_written < (uint64_t)RING_BYTES * 2,
          "dump is a bounded window, not the whole run: %" PRIu64 " bytes",
          s.bytes_written);

    pf_trace_close(t);

    /* And it must be the RECENT window: the trigger marker is in the file
     * and the startup marker is long gone. */
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "reopen %s", path);
    if (f) {
        static char buf[4 * 1024 * 1024];
        size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        bool has_trigger = memmem(buf, n, "perf_drop_detected", 18) != NULL;
        bool has_early = memmem(buf, n, "early_marker", 12) != NULL;
        printf("  trigger marker present=%d, startup marker present=%d\n",
               has_trigger, has_early);
        CHECK(has_trigger, "window contains the event that triggered the dump");
        CHECK(!has_early, "window has recycled past the start of the run");
    }
    printf("  ok\n");
}

/* Continuous mode under wrap pressure, from several threads at once.
 *
 * Producers here are a worst case: a tight loop emitting as fast as the
 * library allows, with no work in between. Four such threads can outrun
 * any single-threaded encoder, so the useful question is not "are there
 * zero drops" but "does the drain thread sustain its throughput and
 * account for every event". A realistic caller traces around actual work
 * and stays far below this rate; see the paced check below. */
static void test_continuous_gapless(void)
{
    printf("continuous, saturated (4 threads, %d events each):\n", EVENTS / 4);

    const char *path = "stress_continuous.pftrace";
    unlink(path);

    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = path;
    cfg.mode = PF_MODE_CONTINUOUS;
    cfg.ring_bytes = 1024 * 1024;
    cfg.watermark_pct = 50;
    cfg.drain_interval_ms = 2;

    struct pf_trace *t = pf_trace_open(&cfg);
    CHECK(t != NULL, "open");
    if (!t)
        return;

    uint64_t proc = pf_track_process(t, 4322, "stress");
    uint64_t tracks[4];
    pthread_t th[4];
    struct worker_arg args[4];
    for (unsigned i = 0; i < 4; i++) {
        char name[16];
        snprintf(name, sizeof(name), "w%u", i);
        tracks[i] = pf_track_thread(t, proc, 4322, (int32_t)(i + 1), name);
        args[i] =
            (struct worker_arg){ .t = t, .track = tracks[i], .n = EVENTS / 4 };
        CHECK(pthread_create(&th[i], NULL, worker_fn, &args[i]) == 0,
              "spawn %u", i);
    }
    for (unsigned i = 0; i < 4; i++)
        pthread_join(th[i], NULL);

    CHECK(pf_trace_flush(t) == 0, "flush");

    struct pf_stats s;
    pf_trace_stats(t, &s);
    uint64_t expected = (uint64_t)(EVENTS / 4) * 4 * 3; /* begin+counter+end */
    printf("  events=%" PRIu64 " (attempted %" PRIu64 ") written=%" PRIu64
           " dropped=%" PRIu64 " (%.0f%%) resyncs=%" PRIu64 "\n",
           s.events, expected, s.bytes_written, s.dropped,
           100.0 * (double)s.dropped / (double)expected, s.resyncs);

    CHECK(s.events + s.dropped == expected,
          "every attempted event is either published or counted dropped: "
          "%" PRIu64 " + %" PRIu64 " != %" PRIu64,
          s.events, s.dropped, expected);
    CHECK(s.resyncs == 0, "continuous mode never resyncs");
    /* The file must be far larger than the ring: proof it drained
     * repeatedly rather than holding a single window. */
    CHECK(s.bytes_written > cfg.ring_bytes,
          "wrote more than one ringful (%" PRIu64 " bytes vs %zu ring)",
          s.bytes_written, cfg.ring_bytes);

    pf_trace_close(t);
    printf("  ok\n");
}

/* Continuous mode at a rate a real caller would produce: traced work with
 * actual work in it. Nothing may be dropped here -- if it is, the library
 * is unusable for its stated purpose, not merely saturated. */
static void test_continuous_paced(void)
{
    printf("continuous, paced (realistic event rate):\n");

    const char *path = "stress_paced.pftrace";
    unlink(path);

    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = path;
    cfg.mode = PF_MODE_CONTINUOUS;
    cfg.ring_bytes = 1024 * 1024;
    cfg.watermark_pct = 75;
    cfg.drain_interval_ms = 10;

    struct pf_trace *t = pf_trace_open(&cfg);
    CHECK(t != NULL, "open");
    if (!t)
        return;

    uint64_t proc = pf_track_process(t, 4324, "paced");
    uint64_t track = pf_track_thread(t, proc, 4324, 1, "main");

    /* ~100k events over ~2s: 50k events/s, far above most real tracing
     * and far below what the drain thread sustains. */
    const unsigned rounds = 20000;
    for (unsigned i = 0; i < rounds; i++) {
        pf_slice_begin(t, track, "request", "paced");
        pf_counter_set_int(t, track, i % 97);
        pf_instant_event(t, track, "checkpoint", "paced");
        pf_slice_end(t, track);
        if (i % 200 == 0)
            usleep(1000);
    }

    CHECK(pf_trace_flush(t) == 0, "flush");

    struct pf_stats s;
    pf_trace_stats(t, &s);
    uint64_t expected = (uint64_t)rounds * 4;
    printf("  events=%" PRIu64 " (attempted %" PRIu64 ") written=%" PRIu64
           " dropped=%" PRIu64 " resyncs=%" PRIu64 "\n",
           s.events, expected, s.bytes_written, s.dropped, s.resyncs);

    CHECK(s.dropped == 0,
          "a realistic event rate must not drop anything (%" PRIu64 " lost)",
          s.dropped);
    CHECK(s.events == expected, "every event published");
    CHECK(s.bytes_written > cfg.ring_bytes,
          "drained repeatedly rather than buffering one window");

    pf_trace_close(t);

    /* Counters agreeing is not the same as the file being complete, so
     * check the bytes: every counter value 0..96 must appear, and the
     * packet count must match what was emitted. A gap from a wrap that
     * the drain thread mishandled would show up as a missing value even
     * though `dropped` stayed at zero. */
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "reopen %s", path);
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc((size_t)sz);
        CHECK(buf && fread(buf, 1, (size_t)sz, f) == (size_t)sz, "read back");
        fclose(f);
        if (buf) {
            /* Counter values are varint-encoded in field 30 (tag 0xF0 0x01
             * for varint wire type). Rather than decode, count how many
             * distinct small values appear as a 3-byte tag+value sequence,
             * which is unambiguous for values under 128. */
            bool seen[97] = { false };
            unsigned found = 0;
            for (long i = 0; i + 2 < sz; i++) {
                if (buf[i] == 0xF0 && buf[i + 1] == 0x01 && buf[i + 2] < 97) {
                    if (!seen[buf[i + 2]]) {
                        seen[buf[i + 2]] = true;
                        found++;
                    }
                }
            }
            printf("  distinct counter values present: %u/97\n", found);
            CHECK(found == 97,
                  "every counter value survived to the file (%u/97)", found);
            free(buf);
        }
    }
    printf("  ok\n");
}

/* Names longer than the record's 255-byte field must be truncated and
 * counted, never written out of bounds. */
static void test_long_names(void)
{
    printf("long names:\n");

    const char *path = "stress_names.pftrace";
    unlink(path);

    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = path;
    cfg.mode = PF_MODE_SNAPSHOT;
    cfg.ring_bytes = 128 * 1024;

    struct pf_trace *t = pf_trace_open(&cfg);
    CHECK(t != NULL, "open");
    if (!t)
        return;

    uint64_t proc = pf_track_process(t, 4323, "names");
    uint64_t track = pf_track_thread(t, proc, 4323, 1, "main");

    char huge[1024];
    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';

    for (unsigned i = 0; i < 50; i++) {
        pf_slice_begin(t, track, huge, huge);
        pf_slice_end(t, track);
    }
    CHECK(pf_trace_dump(t) == 0, "dump");

    struct pf_stats s;
    pf_trace_stats(t, &s);
    printf("  truncated=%" PRIu64 "\n", s.truncated_names);
    CHECK(s.truncated_names == 100, "both name and category counted per event");

    pf_trace_close(t);
    printf("  ok\n");
}

int main(void)
{
    test_snapshot_window();
    test_continuous_gapless();
    test_continuous_paced();
    test_long_names();

    if (failures) {
        printf("\n%d CHECK(S) FAILED\n", failures);
        return 1;
    }
    printf("\nALL STRESS TESTS PASSED\n");
    return 0;
}
