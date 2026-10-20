/* Every packet that carries a timestamp names its clock.
 *
 * Perfetto reads a packet with no timestamp_clock_id as BOOTTIME. A dump
 * stamped with another clock therefore has to say so on each packet, or
 * Perfetto cannot relate its events to the trace clock and drops them. This
 * dumps a snapshot for each configurable clock and checks the packets' ids
 * by walking the protobuf wire format.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "dump_dir.h"
#include "pftrace.h"

/* TracePacket fields, and the BuiltinClock ids Perfetto uses for the clocks. */
#define FN_TRACE_PACKET 1
#define FN_PACKET_TIMESTAMP 8
#define FN_PACKET_TIMESTAMP_CLOCK_ID 58
#define CLOCK_ID_REALTIME 1
#define CLOCK_ID_MONOTONIC 3
#define CLOCK_ID_BOOTTIME 6
#define FN_PACKET_CLOCK_SNAPSHOT 6
#define FN_CLOCK_SNAPSHOT_CLOCKS 1
#define FN_CLOCK_ID 1

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

/* One field of a protobuf message. A varint keeps its value in @val; a
 * length-delimited field points @bytes at its payload, @size long. */
struct field {
    uint64_t num;
    unsigned wt;
    uint64_t val;
    const unsigned char *bytes;
    size_t size;
};

static bool read_varint(const unsigned char *b, size_t len, size_t *p,
                        uint64_t *out)
{
    uint64_t v = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (*p >= len)
            return false;
        unsigned char c = b[(*p)++];
        v |= (uint64_t)(c & 0x7f) << shift;
        if (!(c & 0x80)) {
            *out = v;
            return true;
        }
    }
    return false;
}

/* Reads the field at *p, advancing past it. Returns false on bad bytes. */
static bool next_field(const unsigned char *b, size_t len, size_t *p,
                       struct field *f)
{
    uint64_t key;
    if (!read_varint(b, len, p, &key))
        return false;
    f->num = key >> 3;
    f->wt = (unsigned)(key & 7);
    switch (f->wt) {
    case 0:
        return read_varint(b, len, p, &f->val);
    case 1:
        if (len - *p < 8)
            return false;
        *p += 8;
        return true;
    case 5:
        if (len - *p < 4)
            return false;
        *p += 4;
        return true;
    case 2: {
        uint64_t size;
        if (!read_varint(b, len, p, &size) || size > len - *p)
            return false;
        f->bytes = b + *p;
        f->size = (size_t)size;
        *p += f->size;
        return true;
    }
    default:
        return false;
    }
}

/* Checks each timestamped packet in a dump names @want as its clock. Returns
 * how many packets had a timestamp, so the caller can tell an empty dump from
 * a passing one, or -1 if the bytes do not parse. */
static int check_dump(const unsigned char *buf, size_t len, uint64_t want)
{
    int timed = 0;
    size_t p = 0;
    struct field top;
    while (p < len) {
        if (!next_field(buf, len, &p, &top))
            return -1;
        if (!top.bytes || top.num != FN_TRACE_PACKET)
            continue;

        bool has_ts = false, has_clock = false;
        uint64_t clock = 0;
        size_t q = 0;
        struct field f;
        while (q < top.size) {
            if (!next_field(top.bytes, top.size, &q, &f))
                return -1;
            if (f.wt == 0 && f.num == FN_PACKET_TIMESTAMP)
                has_ts = true;
            if (f.wt == 0 && f.num == FN_PACKET_TIMESTAMP_CLOCK_ID) {
                has_clock = true;
                clock = f.val;
            }
        }
        if (!has_ts)
            continue;
        timed++;
        CHECK(has_clock && clock == want,
              "packet %d names clock %llu, want %llu", timed,
              (unsigned long long)clock, (unsigned long long)want);
    }
    return timed;
}

static unsigned char buf[1 << 20];

/* True if the ClockSnapshot in @b names clock @id among its readings. */
static bool snapshot_has_clock(const unsigned char *b, size_t len, uint64_t id)
{
    size_t p = 0;
    struct field f;
    while (p < len) {
        if (!next_field(b, len, &p, &f))
            return false;
        if (f.wt != 2 || f.num != FN_CLOCK_SNAPSHOT_CLOCKS)
            continue;
        size_t q = 0;
        struct field c;
        while (q < f.size) {
            if (!next_field(f.bytes, f.size, &q, &c))
                return false;
            if (c.wt == 0 && c.num == FN_CLOCK_ID && c.val == id)
                return true;
        }
    }
    return false;
}

/* True if some ClockSnapshot packet in the dump has a BOOTTIME reading. */
static bool dump_has_boottime(const unsigned char *buf, size_t len)
{
    size_t p = 0;
    struct field top;
    while (p < len) {
        if (!next_field(buf, len, &p, &top))
            return false;
        if (!top.bytes || top.num != FN_TRACE_PACKET)
            continue;
        size_t q = 0;
        struct field f;
        while (q < top.size) {
            if (!next_field(top.bytes, top.size, &q, &f))
                return false;
            if (f.wt == 2 && f.num == FN_PACKET_CLOCK_SNAPSHOT &&
                snapshot_has_clock(f.bytes, f.size, CLOCK_ID_BOOTTIME))
                return true;
        }
    }
    return false;
}

/* Dumps a snapshot taken with @clock_id and checks every packet's clock. */
static void run_clock(clockid_t clock_id, uint64_t want, const char *name)
{
    char dir[64];
    snprintf(dir, sizeof(dir), "packet_clock_%s.d", name);
    dir_reset(dir);

    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.mode = PF_MODE_SNAPSHOT;
    cfg.format = PF_FORMAT_PROTOBUF;
    cfg.path = dir;
    cfg.clock_id = clock_id;

    struct pf_trace *t = pf_trace_open(&cfg);
    if (!t) {
        CHECK(false, "open %s trace", name);
        return;
    }
    pf_slice_begin_g("clock_probe");
    pf_slice_end_g();
    pf_instant_event_g("clock_instant");

    char leaf[512] = "";
    if (pf_trace_dump(t, "snap", leaf, sizeof(leaf)) != 0) {
        CHECK(false, "dump %s trace failed", name);
        pf_trace_close(t);
        return;
    }
    pf_trace_close(t);

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, leaf);
    FILE *f = fopen(path, "rb");
    if (!f) {
        CHECK(false, "cannot open %s", path);
        return;
    }
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    int timed = check_dump(buf, n, want);
    CHECK(timed >= 0, "%s dump does not parse", name);
    CHECK(timed > 0, "%s dump has no timestamped packets", name);
    CHECK(dump_has_boottime(buf, n), "%s snapshot has no BOOTTIME entry", name);
    printf("  %s: %d timestamped packets\n", name, timed);
}

int main(void)
{
    printf("test_packet_clock\n");

    run_clock(CLOCK_REALTIME, CLOCK_ID_REALTIME, "realtime");
    run_clock(CLOCK_MONOTONIC, CLOCK_ID_MONOTONIC, "monotonic");
    run_clock(CLOCK_BOOTTIME, CLOCK_ID_BOOTTIME, "boottime");

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
