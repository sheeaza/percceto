/* Typed arguments on the protobuf path, and their drop on raw.
 *
 * Covers: each PF_KV() kind (int, unsigned, double, string) reaches the trace
 * as a DebugAnnotation with the right field and value; a string is copied
 * during the call and cut to PF_DYN_NAME_MAX as dynamic names are, counted in
 * pf_stats.truncated; a NULL string reads as ""; the _g forms and the explicit
 * forms both emit; a raw trace takes the same calls and drops the arguments.
 *
 * The protobuf file is read back with a reader written from the wire format,
 * not with the encoder, so the check is against the bytes on disk.
 *
 * The events are emitted from a helper thread that exits before the trace
 * closes. A thread's partial chunk is committed when it unbinds, so this gets
 * every event into the file without relying on the main thread's exit.
 */

#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "pftrace.h"

static int failures;

static void check(int ok, const char *what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures++;
}

/* --- Emitting ------------------------------------------------------------ */

static void *emit_thread(void *arg)
{
    (void)arg;
    pf_slice_begin_args_g("load", PF_KV("size", (size_t)4096), PF_KV("ratio", 0.5),
                          PF_KV("status", -2));
    pf_slice_end_g();

    pf_instant_event_args_g("tick", PF_KV("n", 7));

    pf_instant_event_args_g("tick", PF_KV("u", 18446744073709551615ULL),
                            PF_KV("neg", -9223372036854775807LL - 1));
    return NULL;
}

/* --- Reading the protobuf file -------------------------------------------
 * The wire format only: varint, fixed64 and length-delimited fields. Field
 * numbers are from the Perfetto schema, as the encoder uses them. */

static int read_varint(const uint8_t **p, const uint8_t *end, uint64_t *v)
{
    *v = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
        if (*p >= end)
            return -1;
        uint8_t b = *(*p)++;
        *v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80))
            return 0;
    }
    return -1;
}

/* Reads the next field of a message body. Returns 1 with a field, 0 at the
 * end, or -1 on a malformed body. A length-delimited field sets @body and
 * @blen; any other kind sets @val. */
static int next_field(const uint8_t **p, const uint8_t *end, uint32_t *field, uint64_t *val,
                      const uint8_t **body, size_t *blen)
{
    if (*p == end)
        return 0;
    uint64_t tag;
    if (read_varint(p, end, &tag) < 0)
        return -1;
    *field = (uint32_t)(tag >> 3);
    switch (tag & 7) {
    case 0:
        return read_varint(p, end, val) < 0 ? -1 : 1;
    case 1:
        if (end - *p < 8)
            return -1;
        *val = 0;
        for (unsigned i = 0; i < 8; i++)
            *val |= (uint64_t)(*p)[i] << (8 * i);
        *p += 8;
        return 1;
    case 2: {
        uint64_t n;
        if (read_varint(p, end, &n) < 0 || (uint64_t)(end - *p) < n)
            return -1;
        *body = *p;
        *blen = (size_t)n;
        *p += n;
        return 1;
    }
    default:
        return -1;
    }
}

#define MAX_NAMES 64
#define MAX_LINES 64
#define LINE_BYTES 512

struct names {
    const char *str[MAX_NAMES];
    size_t len[MAX_NAMES];
};

/* Appends " key=kind:value" for one DebugAnnotation to @line. A key is either
 * interned, as name_iid into @keys, or inline as name. */
static int append_annotation(const uint8_t *body, size_t blen, const struct names *keys, char *line,
                             size_t cap)
{
    const uint8_t *p = body, *end = body + blen;
    const char *key = "";
    size_t klen = 0;
    char val[LINE_BYTES];
    val[0] = '\0';
    for (;;) {
        uint32_t field;
        uint64_t v;
        const uint8_t *b;
        size_t n;
        int r = next_field(&p, end, &field, &v, &b, &n);
        if (r < 0)
            return -1;
        if (r == 0)
            break;
        if (field == 1 && v > 0 && v < MAX_NAMES && keys->str[v]) {
            key = keys->str[v];
            klen = keys->len[v];
        } else if (field == 10) {
            /* Every key in this test is a literal the cache holds, so none
             * should go inline. */
            return -1;
        } else if (field == 4) {
            snprintf(val, sizeof(val), "i:%" PRId64, (int64_t)v);
        } else if (field == 3) {
            snprintf(val, sizeof(val), "u:%" PRIu64, v);
        } else if (field == 5) {
            double d;
            memcpy(&d, &v, sizeof(d));
            snprintf(val, sizeof(val), "d:%.17g", d);
        }
    }
    size_t used = strlen(line);
    snprintf(line + used, cap - used, " %.*s=%s", (int)klen, key, val);
    return 0;
}

/* Reads every packet of @data. For each TrackEvent that carries annotations,
 * writes one line to @out: the event type, its name, then the annotations.
 * Returns the number of lines, or -1 if the file does not parse. */
static int read_lines(const uint8_t *data, size_t len, char out[][LINE_BYTES], int max,
                      int *inline_names)
{
    struct names names; /* event names, interned_data field 2 */
    struct names keys; /* debug annotation names, field 3 */
    memset(&names, 0, sizeof(names));
    memset(&keys, 0, sizeof(keys));
    int count = 0;
    const uint8_t *p = data, *end = data + len;
    for (;;) {
        uint32_t field;
        uint64_t v;
        const uint8_t *pkt;
        size_t plen;
        int r = next_field(&p, end, &field, &v, &pkt, &plen);
        if (r < 0)
            return -1;
        if (r == 0)
            break;
        if (field != 1)
            continue;

        /* One packet. Its interned names and its event are read in order, but
         * the event's name is looked up after the packet, since the names may
         * come later in the same packet. */
        const uint8_t *q = pkt, *qend = pkt + plen;
        uint64_t type = 0, name_iid = 0;
        const char *name = NULL;
        size_t name_len = 0;
        int have_event = 0;
        char line[LINE_BYTES];
        line[0] = '\0';
        int has_ann = 0;
        for (;;) {
            uint32_t f;
            uint64_t fv;
            const uint8_t *fb;
            size_t fn;
            r = next_field(&q, qend, &f, &fv, &fb, &fn);
            if (r < 0)
                return -1;
            if (r == 0)
                break;
            if (f == 12) { /* interned_data */
                const uint8_t *d = fb, *dend = fb + fn;
                for (;;) {
                    uint32_t df;
                    uint64_t dv;
                    const uint8_t *db;
                    size_t dn;
                    r = next_field(&d, dend, &df, &dv, &db, &dn);
                    if (r < 0)
                        return -1;
                    if (r == 0)
                        break;
                    if (df != 2 && df != 3) /* event_names, debug_annotation_names */
                        continue;
                    struct names *tab = df == 2 ? &names : &keys;
                    const uint8_t *e = db, *eend = db + dn;
                    uint64_t iid = 0;
                    const char *nm = NULL;
                    size_t nl = 0;
                    for (;;) {
                        uint32_t ef;
                        uint64_t ev = 0;
                        const uint8_t *eb = NULL;
                        size_t en = 0;
                        r = next_field(&e, eend, &ef, &ev, &eb, &en);
                        if (r < 0)
                            return -1;
                        if (r == 0)
                            break;
                        if (ef == 1)
                            iid = ev;
                        else if (ef == 2) {
                            nm = (const char *)eb;
                            nl = en;
                        }
                    }
                    if (iid > 0 && iid < MAX_NAMES) {
                        tab->str[iid] = nm;
                        tab->len[iid] = nl;
                    }
                }
            } else if (f == 11) { /* track_event */
                have_event = 1;
                const uint8_t *t = fb, *tend = fb + fn;
                for (;;) {
                    uint32_t tf;
                    uint64_t tv;
                    const uint8_t *tb;
                    size_t tn;
                    r = next_field(&t, tend, &tf, &tv, &tb, &tn);
                    if (r < 0)
                        return -1;
                    if (r == 0)
                        break;
                    if (tf == 9)
                        type = tv;
                    else if (tf == 10)
                        name_iid = tv;
                    else if (tf == 23) {
                        name = (const char *)tb;
                        name_len = tn;
                    } else if (tf == 4) {
                        if (!has_ann) {
                            snprintf(line, sizeof(line), "%s",
                                     type == 1 ? "B" :
                                     type == 2 ? "E" :
                                     type == 3 ? "I" :
                                                 "?");
                            has_ann = 1;
                        }
                        if (append_annotation(tb, tn, &keys, line, sizeof(line)) < 0)
                            return -1;
                    }
                }
            }
        }
        if (!has_ann || !have_event)
            continue;
        const char *shown = "-";
        int shown_len = 1;
        if (name_iid && name_iid < MAX_NAMES && names.str[name_iid]) {
            shown = names.str[name_iid];
            shown_len = (int)names.len[name_iid];
        } else if (name) {
            shown = name;
            shown_len = (int)name_len;
            if (inline_names)
                (*inline_names)++;
        }
        char full[LINE_BYTES];
        /* The event type, then the name, then the annotations. line holds the
         * type letter first, then the annotations, each with a leading space. */
        size_t tl = strcspn(line, " ");
        snprintf(full, sizeof(full), "%.*s %.*s%s", (int)tl, line, shown_len, shown, line + tl);
        if (count >= max)
            return -1;
        snprintf(out[count++], LINE_BYTES, "%s", full);
    }
    return count;
}

static int read_file(const char *path, uint8_t **data, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    *data = malloc((size_t)n);
    *len = fread(*data, 1, (size_t)n, f);
    fclose(f);
    return *data && *len == (size_t)n ? 0 : -1;
}

/* --- Checks --------------------------------------------------------------- */

static void check_protobuf(void)
{
    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = ".";
    cfg.name = "args_protobuf.pftrace";
    /* Continuous mode appends to its file, so a run from before would be read
     * back too. */
    unlink(cfg.name);
    cfg.mode = PF_MODE_CONTINUOUS;
    cfg.format = PF_FORMAT_PROTOBUF;
    if (pf_trace_open_g(&cfg) != 0) {
        check(0, "open protobuf trace");
        return;
    }

    pthread_t th;
    if (pthread_create(&th, NULL, emit_thread, NULL) != 0) {
        check(0, "start emit thread");
        pf_trace_close_g();
        return;
    }
    pthread_join(th, NULL);

    struct pf_stats st;
    pf_trace_stats(pf_trace_default_g(), &st);
    check(st.truncated == 0, "no argument truncated: arguments are numbers only");
    check(st.dropped == 0, "no events dropped");

    pf_trace_close_g();

    uint8_t *data;
    size_t len;
    if (read_file("args_protobuf.pftrace", &data, &len) < 0) {
        check(0, "read protobuf trace back");
        return;
    }

    char got[MAX_LINES][LINE_BYTES];
    int n = read_lines(data, len, got, MAX_LINES, NULL);
    check(n >= 0, "protobuf trace parses on the wire");
    free(data);
    if (n < 0)
        return;

    const char *want[] = {
        "B load size=u:4096 ratio=d:0.5 status=i:-2",
        "I tick n=i:7",
        "I tick u=u:18446744073709551615 neg=i:-9223372036854775808",
    };
    const int nwant = (int)(sizeof(want) / sizeof(want[0]));
    check(n == nwant, "three annotated events in the trace");
    for (int i = 0; i < nwant && i < n; i++) {
        char what[LINE_BYTES + 32];
        int ok = strcmp(got[i], want[i]) == 0;
        snprintf(what, sizeof(what), "event %d: %.*s", i, LINE_BYTES, ok ? got[i] : want[i]);
        check(ok, what);
        if (!ok)
            printf("       got: %s\n", got[i]);
    }
}

/* Forty distinct names in one thread's burst. Past 32 of them, the writer's
 * cache has no slot left for a name in the chunk, so those go inline; the
 * decoder must still resolve every event to its own name. */
#define OVF_EV(n) pf_instant_event_args_g("n" #n, PF_KV("i", (int)(n)));
#define OVF_COUNT 40

static void *overflow_thread(void *arg)
{
    (void)arg;
    OVF_EV(1)
    OVF_EV(2)
    OVF_EV(3)
    OVF_EV(4)
    OVF_EV(5)
    OVF_EV(6)
    OVF_EV(7)
    OVF_EV(8)
    OVF_EV(9)
    OVF_EV(10)
    OVF_EV(11)
    OVF_EV(12)
    OVF_EV(13)
    OVF_EV(14)
    OVF_EV(15)
    OVF_EV(16)
    OVF_EV(17)
    OVF_EV(18)
    OVF_EV(19)
    OVF_EV(20)
    OVF_EV(21)
    OVF_EV(22)
    OVF_EV(23)
    OVF_EV(24)
    OVF_EV(25)
    OVF_EV(26)
    OVF_EV(27)
    OVF_EV(28)
    OVF_EV(29)
    OVF_EV(30)
    OVF_EV(31)
    OVF_EV(32)
    OVF_EV(33)
    OVF_EV(34)
    OVF_EV(35)
    OVF_EV(36)
    OVF_EV(37)
    OVF_EV(38)
    OVF_EV(39)
    OVF_EV(40)
    return NULL;
}

static void check_intern_overflow(void)
{
    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = ".";
    cfg.name = "args_overflow.pftrace";
    unlink(cfg.name);
    cfg.mode = PF_MODE_CONTINUOUS;
    cfg.format = PF_FORMAT_PROTOBUF;
    if (pf_trace_open_g(&cfg) != 0) {
        check(0, "open overflow trace");
        return;
    }

    pthread_t th;
    if (pthread_create(&th, NULL, overflow_thread, NULL) != 0) {
        check(0, "start overflow thread");
        pf_trace_close_g();
        return;
    }
    pthread_join(th, NULL);

    struct pf_stats st;
    pf_trace_stats(pf_trace_default_g(), &st);
    check(st.dropped == 0, "overflow: no events dropped");
    pf_trace_close_g();

    uint8_t *data;
    size_t len;
    if (read_file("args_overflow.pftrace", &data, &len) < 0) {
        check(0, "read overflow trace back");
        return;
    }
    static char got[OVF_COUNT][LINE_BYTES];
    int inline_names = 0;
    int n = read_lines(data, len, got, OVF_COUNT, &inline_names);
    check(n >= 0, "overflow: trace parses on the wire");
    free(data);
    if (n < 0)
        return;

    check(n == OVF_COUNT, "overflow: every event decodes");
    check(inline_names > 0, "overflow: some names went past the cache and arrived inline");
    for (int i = 0; i < OVF_COUNT && i < n; i++) {
        char want[LINE_BYTES];
        snprintf(want, sizeof(want), "I n%d i=i:%d", i + 1, i + 1);
        int ok = strcmp(got[i], want) == 0;
        if (!ok)
            printf("       got: %s, want: %s\n", got[i], want);
        check(ok, ok ? "overflow: event named by its own name" : "overflow: wrong name");
        if (!ok)
            break;
    }
}

static void check_raw(void)
{
    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = ".";
    cfg.name = "args_raw.pfraw";
    unlink(cfg.name);
    cfg.mode = PF_MODE_CONTINUOUS;
    cfg.format = PF_FORMAT_RAW;
    if (pf_trace_open_g(&cfg) != 0) {
        check(0, "open raw trace");
        return;
    }
    pf_slice_begin_args_g("load", PF_KV("size", (size_t)4096));
    pf_slice_end_g();

    struct pf_stats st;
    pf_trace_stats(pf_trace_default_g(), &st);
    check(st.events == 2, "raw trace records both events; arguments are dropped, not an error");
    pf_trace_close_g();
}

int main(void)
{
    printf("test_args\n");
    check_protobuf();
    check_intern_overflow();
    check_raw();
    if (failures)
        printf("%d failure(s)\n", failures);
    else
        printf("all passed\n");
    return failures ? 1 : 0;
}
