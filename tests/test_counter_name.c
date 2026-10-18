/* pf_track_counter() and pf_track_thread_named() identify a track by its
 * parent and name.
 *
 * Covers: the same identity returns the same uuid and takes no table entry;
 * distinct names and parents get distinct tracks; a unit conflict returns 0;
 * concurrent creation of one name yields one track; a full table still
 * refuses new names.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "pftrace.h"

#define NTHREADS 4
#define CREATES_PER_THREAD 500

static int failures;

static void check(int ok, const char *what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures++;
}

/* Opens a trace whose table holds @max_tracks entries beyond the process
 * track pf_trace_open() registers, so the tests count only their own. */
static struct pf_trace *open_trace(const char *path, unsigned max_tracks)
{
    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = path;
    cfg.max_tracks = max_tracks + 1;
    return pf_trace_open(&cfg);
}

/* Same name, many calls: one uuid, and the table holds one entry. */
static void test_same_name_one_track(void)
{
    const char *path = "counter_name_same.pftrace";
    struct pf_trace *t = open_trace(path, 4);
    if (!t) {
        check(0, "open trace");
        return;
    }
    uint64_t first = pf_track_counter(t, 0, "queue/a.depth", PF_UNIT_COUNT);
    int same = first != 0;
    for (int i = 0; i < 100; i++)
        same &= pf_track_counter(t, 0, "queue/a.depth", PF_UNIT_COUNT) == first;
    check(same, "same name returns the same uuid");

    /* The table has room for 4. One is taken by the name above, so three
     * more distinct names must fit and a fourth must not. */
    int fits = pf_track_counter(t, 0, "b", PF_UNIT_COUNT) &&
               pf_track_counter(t, 0, "c", PF_UNIT_COUNT) &&
               pf_track_counter(t, 0, "d", PF_UNIT_COUNT);
    check(fits, "repeated name used one table entry, three more fit");
    check(pf_track_counter(t, 0, "e", PF_UNIT_COUNT) == 0,
          "table full refuses a new name");
    check(pf_track_counter(t, 0, "queue/a.depth", PF_UNIT_COUNT) == first,
          "existing name still found on a full table");
    pf_trace_close(t);
    unlink(path);
}

/* Distinct names and distinct parents are separate tracks. */
static void test_distinct_names_and_parents(void)
{
    const char *path = "counter_name_distinct.pftrace";
    struct pf_trace *t = open_trace(path, 16);
    if (!t) {
        check(0, "open trace");
        return;
    }
    uint64_t a = pf_track_counter(t, 0, "queue/a.depth", PF_UNIT_COUNT);
    uint64_t b = pf_track_counter(t, 0, "queue/b.depth", PF_UNIT_COUNT);
    check(a && b && a != b, "distinct names give distinct tracks");

    uint64_t p1 = pf_track_process(t, 1, "p1");
    uint64_t p2 = pf_track_process(t, 2, "p2");
    uint64_t c1 = pf_track_counter(t, p1, "depth", PF_UNIT_COUNT);
    uint64_t c2 = pf_track_counter(t, p2, "depth", PF_UNIT_COUNT);
    check(c1 && c2 && c1 != c2,
          "same name under different parents gives distinct tracks");
    check(pf_track_counter(t, p1, "depth", PF_UNIT_COUNT) == c1,
          "same name under the same parent gives the same track");
    pf_trace_close(t);
    unlink(path);
}

/* Same name, different unit: conflict, reported as 0. */
static void test_unit_conflict(void)
{
    const char *path = "counter_name_unit.pftrace";
    struct pf_trace *t = open_trace(path, 16);
    if (!t) {
        check(0, "open trace");
        return;
    }
    uint64_t ok = pf_track_counter(t, 0, "bytes", PF_UNIT_SIZE_BYTES);
    check(ok != 0, "first registration with a unit succeeds");
    check(pf_track_counter(t, 0, "bytes", PF_UNIT_COUNT) == 0,
          "same name with a different unit returns 0");
    check(pf_track_counter(t, 0, "bytes", PF_UNIT_SIZE_BYTES) == ok,
          "same name with the original unit still returns the track");
    pf_trace_close(t);
    unlink(path);
}

/* Concurrent creation of one name: every thread sees the same uuid. */
struct race_arg {
    struct pf_trace *t;
    uint64_t seen[CREATES_PER_THREAD];
};

static void *race_main(void *arg)
{
    struct race_arg *ra = arg;
    for (int i = 0; i < CREATES_PER_THREAD; i++)
        ra->seen[i] = pf_track_counter(ra->t, 0, "shared", PF_UNIT_COUNT);
    return NULL;
}

static void test_concurrent_same_name(void)
{
    const char *path = "counter_name_race.pftrace";
    struct pf_trace *t = open_trace(path, 16);
    if (!t) {
        check(0, "open trace");
        return;
    }
    pthread_t th[NTHREADS];
    struct race_arg ra[NTHREADS];
    for (int i = 0; i < NTHREADS; i++) {
        ra[i].t = t;
        pthread_create(&th[i], NULL, race_main, &ra[i]);
    }
    for (int i = 0; i < NTHREADS; i++)
        pthread_join(th[i], NULL);

    uint64_t want = ra[0].seen[0];
    int agree = want != 0;
    for (int i = 0; i < NTHREADS; i++)
        for (int j = 0; j < CREATES_PER_THREAD; j++)
            agree &= ra[i].seen[j] == want;
    check(agree, "concurrent creation of one name yields one uuid");

    /* Only one table entry was taken, so 15 more distinct names fit. */
    int room = 1;
    for (int i = 0; i < 15; i++) {
        char name[16];
        snprintf(name, sizeof(name), "n%d", i);
        room &= pf_track_counter(t, 0, name, PF_UNIT_COUNT) != 0;
    }
    check(room, "one table entry for the shared name, 15 more fit");
    pf_trace_close(t);
    unlink(path);
}

/* Thread tracks by name: parent and name identify the track. A repeat call
 * returns the same track whatever pid and tid it passes, a different parent
 * gives a separate track, and a full table refuses new names. */
static void test_thread_named(void)
{
    const char *path = "counter_name_thread.pftrace";
    struct pf_trace *t = open_trace(path, 4);
    if (!t) {
        check(0, "open trace");
        return;
    }
    uint64_t a = pf_track_thread_named(t, 0, 100, 7, "worker");
    int same = a != 0;
    for (int i = 0; i < 1000; i++)
        same &= pf_track_thread_named(t, 0, 100, 1000 + i, "worker") == a;
    check(same, "same name returns the same track, pid and tid ignored");

    uint64_t b = pf_track_thread_named(t, 0, 100, 7, "other");
    check(b && b != a, "different name gives a separate track");

    /* Table has room for 4. Two entries are taken, so two more names fit. */
    int fits = pf_track_thread_named(t, 0, 100, 7, "c") &&
               pf_track_thread_named(t, 0, 100, 7, "d");
    check(fits, "two more names fit");
    check(pf_track_thread_named(t, 0, 100, 7, "e") == 0,
          "table full refuses a new name");
    check(pf_track_thread_named(t, 0, 100, 7, "worker") == a,
          "existing name still found on a full table");
    pf_trace_close(t);
    unlink(path);

    struct pf_trace *u = open_trace(path, 16);
    if (!u) {
        check(0, "open trace");
        return;
    }
    uint64_t p1 = pf_track_process(u, 1, "p1");
    uint64_t p2 = pf_track_process(u, 2, "p2");
    uint64_t x = pf_track_thread_named(u, p1, 1, 7, "worker");
    uint64_t y = pf_track_thread_named(u, p2, 2, 8, "worker");
    check(x && y && x != y, "same name under different parents is separate");
    check(pf_track_thread_named(u, p1, 1, 9, "worker") == x,
          "same name under the same parent is the same track");
    pf_trace_close(u);
    unlink(path);
}

int main(void)
{
    printf("test_counter_name\n");
    test_same_name_one_track();
    test_distinct_names_and_parents();
    test_unit_conflict();
    test_concurrent_same_name();
    test_thread_named();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
