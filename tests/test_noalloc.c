/* Proves requirement 1: no dynamic allocation on the hot path.
 *
 * Interposes the allocator and aborts on any call made while armed, so the
 * check is not "we counted roughly the same number of mallocs" but "a
 * malloc here is a hard failure". Covers both modes, because continuous
 * mode additionally runs the drain thread's encode-and-write path while
 * armed -- the steady-state draining must also be allocation-free, or a
 * long-running trace would fault in the background instead of on the
 * caller's thread.
 *
 * Run as a separate executable rather than a unit test so the interposer
 * cannot affect anything else.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pftrace.h"

static atomic_bool armed;
static atomic_uint_least64_t violations;

/* Must not allocate, so write(2) directly rather than going through
 * stdio, which may allocate its buffer on first use. */
static void report(const char *what)
{
    if (atomic_load(&armed)) {
        atomic_fetch_add(&violations, 1);
        char buf[128];
        size_t n = (size_t)snprintf(buf, sizeof(buf),
                                    "  VIOLATION: %s while armed\n", what);
        ssize_t ignored = write(2, buf, n);
        (void)ignored;
    }
}

void *malloc(size_t n)
{
    static void *(*real)(size_t);
    if (!real)
        real = dlsym(RTLD_NEXT, "malloc");
    report("malloc");
    return real(n);
}

void *calloc(size_t a, size_t b)
{
    static void *(*real)(size_t, size_t);
    if (!real)
        real = dlsym(RTLD_NEXT, "calloc");
    report("calloc");
    return real(a, b);
}

void *realloc(void *p, size_t n)
{
    static void *(*real)(void *, size_t);
    if (!real)
        real = dlsym(RTLD_NEXT, "realloc");
    report("realloc");
    return real(p, n);
}

static int failures;

static void check_mode(const char *label, enum pf_mode mode, bool dump)
{
    printf("%s:\n", label);

    char path[64];
    snprintf(path, sizeof(path), "noalloc_%s.pftrace", label);
    unlink(path);

    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = path;
    cfg.mode = mode;
    cfg.ring_bytes = 512 * 1024;
    cfg.watermark_pct = 50;
    cfg.drain_interval_ms = 2;

    /* Everything before arming is allowed to allocate: that is the point
     * of doing it up front. */
    struct pf_trace *t = pf_trace_open(&cfg);
    if (!t) {
        perror("  pf_trace_open");
        failures++;
        return;
    }
    uint64_t proc = pf_track_process(t, 999, "noalloc");
    uint64_t track = pf_track_thread(t, proc, 999, 1, "main");
    uint64_t ctr = pf_track_counter(t, proc, "depth", PF_UNIT_COUNT);

    /* Touch every distinct event shape once before arming, so lazily
     * initialized libc state (stdio buffers, locale, the first
     * clock_gettime) is not mistaken for per-event allocation. */
    pf_slice_begin(t, track, "warm", "warm");
    pf_slice_end(t, track);
    pf_instant_event(t, track, "warm", "warm");
    pf_counter_set_int(t, ctr, 1);
    pf_counter_set_double(t, ctr, 1.0);
    if (dump)
        pf_trace_dump(t);

    atomic_store(&armed, true);

    /* Enough events to wrap the ring several times over, so continuous
     * mode drains repeatedly and snapshot mode trims repeatedly, all
     * while armed. */
    const unsigned n = 200000;
    for (unsigned i = 0; i < n; i++) {
        pf_slice_begin(t, track, "compute", "app");
        pf_counter_set_int(t, ctr, (int64_t)i);
        pf_instant_event(t, track, "mark", "app");
        pf_counter_set_double(t, ctr, (double)i * 1.5);
        pf_slice_end(t, track);
    }

    /* An explicit dump exercises encode+write on this thread too. */
    if (dump && pf_trace_dump(t) != 0) {
        perror("  pf_trace_dump");
        failures++;
    }

    atomic_store(&armed, false);

    uint64_t v = atomic_load(&violations);
    struct pf_stats s;
    pf_trace_stats(t, &s);
    printf("  %u events emitted, %" PRIu64 " published, %" PRIu64 " written\n",
           n * 5, s.events, s.bytes_written);
    if (v == 0) {
        printf("  ok: zero allocations while armed\n");
    } else {
        printf("  FAIL: %" PRIu64 " allocation(s) on the hot path\n", v);
        failures++;
    }
    atomic_store(&violations, 0);

    pf_trace_close(t);
    unlink(path);
}

int main(void)
{
    /* Warm the interposer's dlsym lookups and stdio before arming. */
    void *p = malloc(1);
    free(p);
    printf("no-allocation check\n");

    check_mode("snapshot", PF_MODE_SNAPSHOT, true);
    check_mode("continuous", PF_MODE_CONTINUOUS, false);

    if (failures) {
        printf("\n%d CHECK(S) FAILED\n", failures);
        return 1;
    }
    printf("\nNO ALLOCATIONS ON THE HOT PATH\n");
    return 0;
}
