/* Validates a trace against the protobuf wire format, with no external
 * dependencies.
 *
 * tools/verify_trace.py is the stronger check -- it parses with Google's
 * own generated code from the real Perfetto schema -- but it needs the
 * perfetto Python package, which is not installable in every environment.
 * This covers the properties that actually break when a ring buffer is
 * involved: truncated or torn packets, events referring to tracks whose
 * descriptors were overwritten, unresolvable interning iids, and
 * out-of-order timestamps. */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Field numbers, mirroring src/pftrace.c. */
#define FN_TRACE_PACKET 1
#define FN_PACKET_TIMESTAMP 8
#define FN_PACKET_TRUSTED_SEQ_ID 10
#define FN_PACKET_TRACK_EVENT 11
#define FN_PACKET_INTERNED_DATA 12
#define FN_PACKET_SEQUENCE_FLAGS 13
#define FN_PACKET_TRACK_DESCRIPTOR 60

#define SEQ_INCREMENTAL_STATE_CLEARED 1u

#define FN_EVENT_CATEGORY_IIDS 3
#define FN_EVENT_TYPE 9
#define FN_EVENT_NAME_IID 10
#define FN_EVENT_TRACK_UUID 11
#define FN_EVENT_COUNTER_VALUE 30
#define FN_EVENT_CATEGORIES_STR 22
#define FN_EVENT_NAME_STR 23
#define FN_EVENT_DOUBLE_COUNTER_VALUE 44

#define TYPE_SLICE_BEGIN 1
#define TYPE_SLICE_END 2
#define TYPE_INSTANT 3
#define TYPE_COUNTER 4

#define FN_TRACK_UUID 1
#define FN_INTERNED_EVENT_CATEGORIES 1
#define FN_INTERNED_EVENT_NAMES 2
#define FN_IID 1

#define MAX_TRACKS 4096
#define MAX_IIDS 65536

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

struct cursor {
    const uint8_t *p;
    const uint8_t *end;
    bool bad;
};

static uint64_t get_varint(struct cursor *c)
{
    uint64_t v = 0;
    unsigned shift = 0;
    while (c->p < c->end) {
        uint8_t b = *c->p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80))
            return v;
        shift += 7;
        if (shift > 63)
            break;
    }
    c->bad = true;
    return 0;
}

/* Skips a field whose tag has already been consumed. */
static bool skip_field(struct cursor *c, unsigned wire_type)
{
    switch (wire_type) {
    case 0:
        get_varint(c);
        return !c->bad;
    case 1:
        if (c->end - c->p < 8)
            return false;
        c->p += 8;
        return true;
    case 2: {
        uint64_t len = get_varint(c);
        if (c->bad || (uint64_t)(c->end - c->p) < len)
            return false;
        c->p += len;
        return true;
    }
    case 5:
        if (c->end - c->p < 4)
            return false;
        c->p += 4;
        return true;
    default:
        return false;
    }
}

struct state {
    uint64_t tracks[MAX_TRACKS];
    size_t n_tracks;

    /* Interning is per-sequence and reset by INCREMENTAL_STATE_CLEARED,
     * so a snapshot dump's window resolves independently of earlier ones. */
    bool name_iid[MAX_IIDS];
    bool cat_iid[MAX_IIDS];

    uint64_t depth[MAX_TRACKS]; /* open slices per track, parallel to tracks */
    uint64_t last_ts;

    uint64_t n_packets, n_begin, n_end, n_instant, n_counter, n_descriptors;
    uint64_t n_state_clears;
    uint64_t unknown_track, unresolved_name, unresolved_cat, ts_regressions;
    uint64_t max_ts_regression;
    uint64_t unbalanced_end;
};

static size_t track_index(struct state *s, uint64_t uuid)
{
    for (size_t i = 0; i < s->n_tracks; i++)
        if (s->tracks[i] == uuid)
            return i;
    return (size_t)-1;
}

static void track_add(struct state *s, uint64_t uuid)
{
    if (track_index(s, uuid) != (size_t)-1)
        return;
    if (s->n_tracks < MAX_TRACKS) {
        s->tracks[s->n_tracks] = uuid;
        s->depth[s->n_tracks] = 0;
        s->n_tracks++;
    }
}

static void parse_interned(struct state *s, const uint8_t *p, size_t len)
{
    struct cursor c = { p, p + len, false };
    while (c.p < c.end && !c.bad) {
        uint64_t tag = get_varint(&c);
        if (c.bad)
            break;
        unsigned field = (unsigned)(tag >> 3);
        unsigned wt = (unsigned)(tag & 7);
        if (wt == 2 && (field == FN_INTERNED_EVENT_NAMES ||
                        field == FN_INTERNED_EVENT_CATEGORIES)) {
            uint64_t sub_len = get_varint(&c);
            if (c.bad || (uint64_t)(c.end - c.p) < sub_len)
                break;
            struct cursor e = { c.p, c.p + sub_len, false };
            uint64_t iid = 0;
            while (e.p < e.end && !e.bad) {
                uint64_t etag = get_varint(&e);
                if (e.bad)
                    break;
                if ((etag >> 3) == FN_IID && (etag & 7) == 0)
                    iid = get_varint(&e);
                else if (!skip_field(&e, (unsigned)(etag & 7)))
                    break;
            }
            if (iid && iid < MAX_IIDS) {
                if (field == FN_INTERNED_EVENT_NAMES)
                    s->name_iid[iid] = true;
                else
                    s->cat_iid[iid] = true;
            }
            c.p += sub_len;
        } else if (!skip_field(&c, wt)) {
            break;
        }
    }
}

static void parse_track_event(struct state *s, const uint8_t *p, size_t len,
                              uint64_t ts)
{
    struct cursor c = { p, p + len, false };
    uint64_t type = 0, track_uuid = 0, name_iid = 0, cat_iid = 0;
    bool has_name_str = false, has_cat_str = false;

    while (c.p < c.end && !c.bad) {
        uint64_t tag = get_varint(&c);
        if (c.bad)
            break;
        unsigned field = (unsigned)(tag >> 3);
        unsigned wt = (unsigned)(tag & 7);

        if (wt == 0) {
            uint64_t v = get_varint(&c);
            switch (field) {
            case FN_EVENT_TYPE:
                type = v;
                break;
            case FN_EVENT_TRACK_UUID:
                track_uuid = v;
                break;
            case FN_EVENT_NAME_IID:
                name_iid = v;
                break;
            case FN_EVENT_CATEGORY_IIDS:
                cat_iid = v;
                break;
            case FN_EVENT_COUNTER_VALUE:
                break;
            default:
                break;
            }
        } else if (wt == 2 && field == FN_EVENT_NAME_STR) {
            has_name_str = true;
            if (!skip_field(&c, wt))
                break;
        } else if (wt == 2 && field == FN_EVENT_CATEGORIES_STR) {
            has_cat_str = true;
            if (!skip_field(&c, wt))
                break;
        } else if (!skip_field(&c, wt)) {
            break;
        }
    }

    (void)has_cat_str;

    size_t ti = track_index(s, track_uuid);
    if (ti == (size_t)-1)
        s->unknown_track++;

    if (name_iid && name_iid < MAX_IIDS && !s->name_iid[name_iid] &&
        !has_name_str)
        s->unresolved_name++;
    if (cat_iid && cat_iid < MAX_IIDS && !s->cat_iid[cat_iid])
        s->unresolved_cat++;

    if (ts < s->last_ts) {
        s->ts_regressions++;
        uint64_t delta = s->last_ts - ts;
        if (delta > s->max_ts_regression)
            s->max_ts_regression = delta;
    }
    s->last_ts = ts;

    switch (type) {
    case TYPE_SLICE_BEGIN:
        s->n_begin++;
        if (ti != (size_t)-1)
            s->depth[ti]++;
        break;
    case TYPE_SLICE_END:
        s->n_end++;
        if (ti != (size_t)-1) {
            if (s->depth[ti] == 0)
                s->unbalanced_end++;
            else
                s->depth[ti]--;
        }
        break;
    case TYPE_INSTANT:
        s->n_instant++;
        break;
    case TYPE_COUNTER:
        s->n_counter++;
        break;
    default:
        break;
    }
}

static void parse_packet(struct state *s, const uint8_t *p, size_t len)
{
    struct cursor c = { p, p + len, false };
    uint64_t ts = 0, flags = 0;
    const uint8_t *event = NULL, *interned = NULL, *descriptor = NULL;
    size_t event_len = 0, interned_len = 0, descriptor_len = 0;

    while (c.p < c.end && !c.bad) {
        uint64_t tag = get_varint(&c);
        if (c.bad)
            break;
        unsigned field = (unsigned)(tag >> 3);
        unsigned wt = (unsigned)(tag & 7);

        if (wt == 0) {
            uint64_t v = get_varint(&c);
            if (field == FN_PACKET_TIMESTAMP)
                ts = v;
            else if (field == FN_PACKET_SEQUENCE_FLAGS)
                flags = v;
            else if (field == FN_PACKET_TRUSTED_SEQ_ID)
                CHECK(v == 1, "unexpected trusted_packet_sequence_id %" PRIu64,
                      v);
        } else if (wt == 2) {
            uint64_t sub_len = get_varint(&c);
            if (c.bad || (uint64_t)(c.end - c.p) < sub_len) {
                c.bad = true;
                break;
            }
            if (field == FN_PACKET_TRACK_EVENT) {
                event = c.p;
                event_len = sub_len;
            } else if (field == FN_PACKET_INTERNED_DATA) {
                interned = c.p;
                interned_len = sub_len;
            } else if (field == FN_PACKET_TRACK_DESCRIPTOR) {
                descriptor = c.p;
                descriptor_len = sub_len;
            }
            c.p += sub_len;
        } else if (!skip_field(&c, wt)) {
            c.bad = true;
            break;
        }
    }
    CHECK(!c.bad, "malformed TracePacket at offset");

    /* A state clear restarts interning for the sequence; anything still
     * referring to an older iid afterwards would be unreadable. */
    if (flags & SEQ_INCREMENTAL_STATE_CLEARED) {
        memset(s->name_iid, 0, sizeof(s->name_iid));
        memset(s->cat_iid, 0, sizeof(s->cat_iid));
        s->n_state_clears++;
    }

    if (descriptor) {
        struct cursor d = { descriptor, descriptor + descriptor_len, false };
        uint64_t uuid = 0;
        while (d.p < d.end && !d.bad) {
            uint64_t tag = get_varint(&d);
            if (d.bad)
                break;
            if ((tag >> 3) == FN_TRACK_UUID && (tag & 7) == 0)
                uuid = get_varint(&d);
            else if (!skip_field(&d, (unsigned)(tag & 7)))
                break;
        }
        if (uuid)
            track_add(s, uuid);
        s->n_descriptors++;
    }

    /* Interning must be processed before the event that uses it. */
    if (interned)
        parse_interned(s, interned, interned_len);
    if (event)
        parse_track_event(s, event, event_len, ts);

    s->n_packets++;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <trace-file>\n", argv[0]);
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fprintf(stderr, "%s: empty\n", argv[1]);
        fclose(f);
        return 1;
    }
    uint8_t *data = malloc((size_t)size);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "%s: read failed\n", argv[1]);
        free(data);
        fclose(f);
        return 1;
    }
    fclose(f);

    struct state *s = calloc(1, sizeof(*s));
    if (!s) {
        free(data);
        return 1;
    }

    /* The file is a bare sequence of length-delimited Trace.packet
     * entries. Framing must consume it exactly -- a short final record
     * would mean a torn write. */
    struct cursor c = { data, data + size, false };
    while (c.p < c.end) {
        uint64_t tag = get_varint(&c);
        if (c.bad) {
            CHECK(false, "truncated tag at byte %ld", (long)(c.p - data));
            break;
        }
        unsigned field = (unsigned)(tag >> 3);
        unsigned wt = (unsigned)(tag & 7);
        if (field != FN_TRACE_PACKET || wt != 2) {
            CHECK(false, "unexpected top-level field %u wire type %u at %ld",
                  field, wt, (long)(c.p - data));
            break;
        }
        uint64_t len = get_varint(&c);
        if (c.bad || (uint64_t)(c.end - c.p) < len) {
            CHECK(false, "truncated packet at byte %ld (want %" PRIu64 ")",
                  (long)(c.p - data), len);
            break;
        }
        parse_packet(s, c.p, len);
        c.p += len;
    }
    CHECK(c.p == c.end, "framing consumed the file exactly");

    printf("parsed: %" PRIu64 " packets, %ld bytes\n", s->n_packets, size);
    printf("track descriptors: %" PRIu64 " (%zu distinct uuids)\n",
           s->n_descriptors, s->n_tracks);
    printf("state clears: %" PRIu64 "\n", s->n_state_clears);
    printf("slice begin: %" PRIu64 "  slice end: %" PRIu64 "\n", s->n_begin,
           s->n_end);
    printf("instant: %" PRIu64 "  counter: %" PRIu64 "\n", s->n_instant,
           s->n_counter);

    CHECK(s->n_packets > 0, "trace contains packets");
    CHECK(s->n_tracks > 0, "trace declares at least one track");
    CHECK(s->unknown_track == 0,
          "%" PRIu64 " events referenced a track with no descriptor",
          s->unknown_track);
    CHECK(s->unresolved_name == 0,
          "%" PRIu64 " events used an unresolvable name_iid",
          s->unresolved_name);
    CHECK(s->unresolved_cat == 0,
          "%" PRIu64 " events used an unresolvable category iid",
          s->unresolved_cat);
    /* Slices left open at the end of a window are expected -- a snapshot
     * cuts the trace mid-flight -- so only report the depth. */
    uint64_t still_open = 0;
    for (size_t i = 0; i < s->n_tracks; i++)
        still_open += s->depth[i];
    printf("slices still open at end of trace: %" PRIu64 "\n", still_open);

    /* Slice ends with no begin, and timestamps that step backwards, are
     * both expected in a ring-buffered trace and are reported rather than
     * failed:
     *
     *   - A window starts wherever the ring had recycled to, so the first
     *     events on a track can be ends whose begins are long gone. Only
     *     a continuous trace read from its start should balance exactly.
     *
     *   - Events are timestamped before claiming ring space, so two
     *     threads can land in the ring in the opposite order to their
     *     timestamps. Perfetto sorts by timestamp, so this is harmless;
     *     what would not be is a timestamp that is wildly wrong, which
     *     shows up as a regression far larger than a scheduling window. */
    printf("orphaned slice ends: %" PRIu64 " (expected when a window starts "
           "mid-slice)\n",
           s->unbalanced_end);
    printf("timestamp inversions: %" PRIu64 " (expected with concurrent "
           "producers; max %" PRIu64 " ns)\n",
           s->ts_regressions, s->max_ts_regression);

    /* A regression beyond a plausible scheduling delay means a torn or
     * misattributed record, not merely racing producers. */
    CHECK(s->max_ts_regression < 1000000000ull,
          "largest timestamp inversion is within a scheduling window "
          "(%" PRIu64 " ns)",
          s->max_ts_regression);

    int rc = failures ? 1 : 0;
    printf(failures ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n",
           failures);
    free(s);
    free(data);
    return rc;
}
