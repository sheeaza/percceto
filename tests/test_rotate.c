/* Output rotation in continuous mode.
 *
 * Covers: with rotation off, one file and no others; with a size limit and no
 * name limit, the files are name, name.1, ... and every file but the last
 * reached the limit before it was closed; with a name limit, the names repeat,
 * the files on disk stay at that count, and each reuse is counted; a raw file
 * set starts each file with its own header.
 *
 * Events come from a helper thread that exits before the flush, so every event
 * is in a committed chunk. The flush writes all of them, and pf_trace_stats is
 * read before close. The files are read back from disk after close.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dump_dir.h"
#include "pftrace.h"

#define EVENTS 20000
#define LIMIT 16384

static int failures;

static void check(int ok, const char *what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures++;
}

static void *emit_thread(void *arg)
{
    struct pf_trace *t = arg;
    uint64_t trk = pf_track_thread(t, (int32_t)getpid(), 1, "emitter");
    for (int i = 0; i < EVENTS; i++)
        pf_instant_event(t, trk, "tick");
    return NULL;
}

/* Opens a continuous trace in @dir, emits EVENTS, flushes, and reads the
 * stats. Returns 0, or -1 if the trace did not open. */
static int run(const char *dir, enum pf_format format, size_t limit, unsigned max_files,
               struct pf_stats *s)
{
    dir_reset(dir);

    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = dir;
    cfg.name = "trace.pftrace";
    cfg.mode = PF_MODE_CONTINUOUS;
    cfg.format = format;
    cfg.max_file_bytes = limit;
    cfg.max_files = max_files;

    struct pf_trace *t = pf_trace_open(&cfg);
    if (!t)
        return -1;

    pthread_t th;
    pthread_create(&th, NULL, emit_thread, t);
    pthread_join(th, NULL);

    pf_trace_flush(t, NULL, NULL, 0);
    pf_trace_stats(t, s);
    pf_trace_close(t);
    return 0;
}

/* Size of the file at slot @slot in @dir, or -1 if there is none. Slot 0 is
 * the configured name, as the library names it. */
static long slot_size(const char *dir, unsigned slot)
{
    char path[512];
    if (slot == 0)
        snprintf(path, sizeof(path), "%s/trace.pftrace", dir);
    else
        snprintf(path, sizeof(path), "%s/trace.pftrace.%u", dir, slot);

    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return (long)st.st_size;
}

/* Number of files in @dir under the rotation names, counting from slot 0 up
 * to the first missing one. */
static unsigned files_on_disk(const char *dir)
{
    unsigned n = 0;
    while (slot_size(dir, n) >= 0)
        n++;
    return n;
}

static void test_unbounded(void)
{
    printf("unbounded rotation, protobuf\n");
    const char *dir = "rotate_unbounded";
    struct pf_stats s;
    if (run(dir, PF_FORMAT_PROTOBUF, LIMIT, 0, &s) != 0) {
        check(0, "trace opens");
        return;
    }

    check(s.files >= 3, "several files written");
    check(files_on_disk(dir) == s.files, "one file on disk per file written");
    check(s.files_overwritten == 0, "no name is reused without a name limit");
    check(slot_size(dir, (unsigned)s.files) < 0, "no file past the last one");

    /* A file closes when the next chunk would take it past the limit, so no
     * file goes over, and each full file is more than half of it: a chunk is
     * about 8 KB here. */
    int within = 1;
    int half_full = 1;
    for (unsigned i = 0; i < s.files; i++) {
        if (slot_size(dir, i) > LIMIT)
            within = 0;
        if (i + 1 < s.files && slot_size(dir, i) * 2 <= LIMIT)
            half_full = 0;
    }
    check(within, "no file is larger than the limit");
    check(half_full, "every file but the last is more than half full when it closes");
}

static void test_off(void)
{
    printf("rotation off\n");
    const char *dir = "rotate_off";
    struct pf_stats s;
    if (run(dir, PF_FORMAT_PROTOBUF, 0, 0, &s) != 0) {
        check(0, "trace opens");
        return;
    }

    check(s.files == 1, "one file written");
    check(slot_size(dir, 0) > 0, "the configured name holds the events");
    check(slot_size(dir, 1) < 0, "no rotated file is made");
}

static void test_ring(void)
{
    printf("name limit, protobuf\n");
    const char *dir = "rotate_ring";
    struct pf_stats s;
    if (run(dir, PF_FORMAT_PROTOBUF, LIMIT, 2, &s) != 0) {
        check(0, "trace opens");
        return;
    }

    check(s.files >= 4, "enough rotations to reuse both names");
    check(files_on_disk(dir) == 2, "only two names on disk");
    check(s.files_overwritten == s.files - 2, "each reuse of a name is counted");
}

static void test_raw_headers(void)
{
    printf("rotation, raw\n");
    const char *dir = "rotate_raw";
    struct pf_stats s;
    if (run(dir, PF_FORMAT_RAW, LIMIT, 0, &s) != 0) {
        check(0, "trace opens");
        return;
    }

    check(s.files >= 2, "several raw files written");
    int headers = 1;
    for (unsigned i = 0; i < s.files; i++) {
        char path[512];
        if (i == 0)
            snprintf(path, sizeof(path), "%s/trace.pftrace", dir);
        else
            snprintf(path, sizeof(path), "%s/trace.pftrace.%u", dir, i);

        FILE *f = fopen(path, "rb");
        char magic[8] = "";
        if (!f || fread(magic, 1, sizeof(magic), f) != sizeof(magic) ||
            memcmp(magic, "PFRAWv1", 7) != 0)
            headers = 0;
        if (f)
            fclose(f);
    }
    check(headers, "every raw file starts with the raw header");
}

int main(void)
{
    test_unbounded();
    test_off();
    test_ring();
    test_raw_headers();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all rotation checks passed\n");
    return 0;
}
