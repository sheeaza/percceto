/* Event arguments in the protobuf format.
 *
 * The drain thread renders each event's arguments into its name. This
 * dumps a snapshot and looks for the rendered names in the file. The name
 * is written as a plain string, so the rendered text appears as bytes; a
 * format that does not match its arguments falls back to the bare format,
 * which is checked too.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "dump_dir.h"
#include "pftrace.h"

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

static bool contains(const unsigned char *buf, size_t len, const char *needle)
{
    size_t n = strlen(needle);
    if (n > len)
        return false;
    for (size_t i = 0; i + n <= len; i++)
        if (memcmp(buf + i, needle, n) == 0)
            return true;
    return false;
}

int main(void)
{
    const char *dir = "args_pb.d";
    dir_reset(dir);

    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.mode = PF_MODE_SNAPSHOT;
    cfg.format = PF_FORMAT_PROTOBUF;
    cfg.path = dir;

    if (pf_trace_open_g(&cfg) != 0) {
        perror("open");
        return 1;
    }
    struct pf_trace *t = pf_trace_default_g();
    uint64_t trk = pf_track_thread(t, getpid(), 1, "main");

    pf_slice_begin_args(t, trk, "job %d took %f", 7, 2.5);
    pf_slice_end(t, trk);
    pf_instant_event_args(t, trk, "n=%u x=%x", 42u, 255u);
    /* A double given to %d does not match its argument: the bare format
     * must be written instead. */
    pf_instant_event_args(t, trk, "bad %d", 2.5);
    /* Width, flags, precision, %c and %p, one event each, so a failure
     * names the conversion that broke. */
    pf_instant_event_args(t, trk, "w[%8d]", 5);
    pf_instant_event_args(t, trk, "L[%-6d]", 3);
    pf_instant_event_args(t, trk, "S[%+d]", 3);
    pf_instant_event_args(t, trk, "c=%c%c", 104, 105);
    pf_instant_event_args(t, trk, "p=%p", 4096);
    pf_instant_event_args(t, trk, "P=%8p", 255);
    pf_instant_event_args(t, trk, "f=%.3f", 2.5);
    pf_instant_event_args(t, trk, "u=%5.2e", 12345.0);
    /* %# is not accepted: the bare format must be written. */
    pf_instant_event_args(t, trk, "hash=%#x", 255u);
    /* _g forms: the calling thread's track, with args too. */
    pf_slice_begin_args_g("g-job %d", 9);
    pf_slice_end_g();
    pf_instant_event_args_g("g-n=%u", 4u);

    char leaf[512] = "";
    if (pf_trace_dump(t, "snap", leaf, sizeof(leaf)) != 0) {
        CHECK(false, "dump failed");
        pf_trace_close_g();
        return 1;
    }
    pf_trace_close_g();

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, leaf);
    FILE *f = fopen(path, "rb");
    if (!f) {
        CHECK(false, "cannot open %s", path);
        return 1;
    }
    static unsigned char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    CHECK(contains(buf, n, "job 7 took 2.500000"), "rendered slice name is missing");
    CHECK(contains(buf, n, "n=42 x=ff"), "rendered instant name is missing");
    CHECK(contains(buf, n, "bad %d"), "mismatched format did not fall back to the bare format");
    CHECK(!contains(buf, n, "job %d took %f"), "unrendered format leaked into the output");
    CHECK(contains(buf, n, "w[       5]"), "8d width not rendered");
    CHECK(contains(buf, n, "L[3     ]"), "-6d left-justify not rendered");
    CHECK(contains(buf, n, "S[+3]"), "+d sign not rendered");
    CHECK(contains(buf, n, "c=hi"), "c not rendered");
    CHECK(contains(buf, n, "p=0x1000"), "p not rendered");
    CHECK(contains(buf, n, "P=    0xff"), "8p width not rendered");
    CHECK(contains(buf, n, "f=2.500"), ".3f precision not rendered");
    CHECK(contains(buf, n, "u=1.23e+04"), "5.2e not rendered");
    CHECK(contains(buf, n, "hash=%#x"),
          "the hash flag was accepted; it must fall back to the bare format");
    CHECK(contains(buf, n, "g-job 9"), "_g slice args not rendered");
    CHECK(contains(buf, n, "g-n=4"), "_g instant args not rendered");

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("args_pb: ok\n");
    return 0;
}
