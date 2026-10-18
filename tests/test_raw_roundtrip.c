/* Round trip for the raw format.
 *
 * Writes a known event sequence through PF_FORMAT_RAW, reads the file back
 * with the reader below, and checks every decoded event against the
 * sequence that was emitted. The reader is written from docs/raw_format.md
 * and shares no code with pftrace.c, so a writer and a reader that agree
 * with each other but not with the spec fail here.
 *
 * Also covers the properties the spec promises: reference ordering (every
 * event's name and track are defined earlier in the file), refusal of a
 * non-empty file, and a torn tail being detected rather than misread.
 */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "pftrace.h"

/* Mirrors the values documented in docs/raw_format.md. Kept separate from
 * pftrace.c on purpose. */
#define REC_BYTES 32u
#define RF_HAS_VALUE 0x1u
enum {
    TYPE_SLICE_BEGIN = 1,
    TYPE_SLICE_END = 2,
    TYPE_INSTANT = 3,
    TYPE_COUNTER = 4
};
enum {
    CHUNK_NAMES = 1,
    CHUNK_TRACKS = 2,
    CHUNK_EVENTS = 3
};

#define MAX_NAMES 16
#define MAX_TRACKS 8
#define NAME_MAX_LEN 64

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

/* --- Reader ------------------------------------------------------------ */

struct rev {
    uint64_t ts, track, u;
    uint32_t type, flags;
};

struct rtrack {
    uint64_t uuid, parent;
    uint32_t kind, unit;
    int32_t pid, tid;
    char name[NAME_MAX_LEN + 1];
};

struct rname {
    uint64_t ptr;
    char s[NAME_MAX_LEN + 1];
};

struct parsed {
    uint32_t version, record_size, clock_id, flags;
    struct rtrack trk[MAX_TRACKS];
    size_t ntrk;
    struct rname nm[MAX_NAMES];
    size_t nnm;
    struct rev *ev;
    size_t nev;
    size_t torn_bytes;
};

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static uint64_t rd64(const uint8_t *p)
{
    return rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

static const struct rname *find_name(const struct parsed *p, uint64_t ptr)
{
    for (size_t i = 0; i < p->nnm; i++)
        if (p->nm[i].ptr == ptr)
            return &p->nm[i];
    return NULL;
}

static size_t find_track_index(const struct parsed *p, uint64_t uuid)
{
    for (size_t i = 0; i < p->ntrk; i++)
        if (p->trk[i].uuid == uuid)
            return i;
    return p->ntrk;
}

static const struct rtrack *find_track(const struct parsed *p, uint64_t uuid)
{
    size_t i = find_track_index(p, uuid);
    return i < p->ntrk ? &p->trk[i] : NULL;
}

/* Returns 0 on a well-formed file, possibly with a torn tail, -1 on a
 * malformed one. Chunks whose references are out of order are malformed. */
static int parse_raw(const char *path, struct parsed *p)
{
    memset(p, 0, sizeof(*p));

    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(size > 0 ? (size_t)size : 1);
    if (!buf || (long)fread(buf, 1, (size_t)size, f) != size) {
        fclose(f);
        free(buf);
        return -1;
    }
    fclose(f);

    int rc = -1;
    if (size < 32 || memcmp(buf, "PFRAWv1\0", 8) != 0) {
        fprintf(stderr, "bad magic or short header\n");
        goto out;
    }
    p->version = rd32(buf + 8);
    p->record_size = rd32(buf + 12);
    p->clock_id = rd32(buf + 16);
    p->flags = rd32(buf + 20);

    p->ev = malloc((size_t)size / REC_BYTES * sizeof(*p->ev) + 1);
    if (!p->ev)
        goto out;

    size_t off = 32;
    while (off < (size_t)size) {
        size_t left = (size_t)size - off;
        if (left < 8) {
            p->torn_bytes = left;
            break;
        }
        uint32_t kind = rd32(buf + off);
        uint32_t len = rd32(buf + off + 4);
        if (left - 8 < len) {
            p->torn_bytes = left;
            break;
        }
        const uint8_t *body = buf + off + 8;
        off += 8 + len;

        if (kind == CHUNK_NAMES) {
            for (size_t q = 0; q < len;) {
                if (len - q < 12)
                    goto out;
                uint64_t ptr = rd64(body + q);
                uint32_t nl = rd32(body + q + 8);
                q += 12;
                if (nl > NAME_MAX_LEN || len - q < nl || p->nnm == MAX_NAMES)
                    goto out;
                const struct rname *have = find_name(p, ptr);
                if (have) {
                    /* A repeat must be identical; first definition wins. */
                    if (strncmp(have->s, (const char *)body + q, nl) != 0 ||
                        have->s[nl] != '\0')
                        goto out;
                } else {
                    struct rname *n = &p->nm[p->nnm++];
                    n->ptr = ptr;
                    memcpy(n->s, body + q, nl);
                    n->s[nl] = '\0';
                }
                q += nl;
            }
        } else if (kind == CHUNK_TRACKS) {
            if (len % 104 != 0)
                goto out;
            for (size_t q = 0; q < len; q += 104) {
                const uint8_t *e = body + q;
                struct rtrack nt;
                nt.uuid = rd64(e);
                nt.parent = rd64(e + 8);
                nt.kind = rd32(e + 16);
                nt.pid = (int32_t)rd32(e + 20);
                nt.tid = (int32_t)rd32(e + 24);
                nt.unit = rd32(e + 28);
                uint32_t nl = rd32(e + 36);
                if (nl > NAME_MAX_LEN)
                    goto out;
                memcpy(nt.name, e + 40, nl);
                nt.name[nl] = '\0';

                size_t at = find_track_index(p, nt.uuid);
                if (at == p->ntrk) {
                    if (p->ntrk == MAX_TRACKS)
                        goto out;
                    p->trk[p->ntrk++] = nt;
                } else {
                    /* A rename: everything but the name must match. */
                    const struct rtrack *have = &p->trk[at];
                    if (have->parent != nt.parent || have->kind != nt.kind ||
                        have->pid != nt.pid || have->tid != nt.tid ||
                        have->unit != nt.unit)
                        goto out;
                    p->trk[at] = nt;
                }
            }
        } else if (kind == CHUNK_EVENTS) {
            if (len % REC_BYTES != 0)
                goto out;
            for (size_t q = 0; q < len; q += REC_BYTES) {
                const uint8_t *e = body + q;
                struct rev *ev = &p->ev[p->nev++];
                ev->ts = rd64(e);
                ev->track = rd64(e + 8);
                ev->u = rd64(e + 16);
                ev->type = rd32(e + 24);
                ev->flags = rd32(e + 28);

                /* Ordering rule: definitions precede use. */
                if (!find_track(p, ev->track))
                    goto out;
                if (!(ev->flags & RF_HAS_VALUE) && ev->u != 0 &&
                    !find_name(p, ev->u))
                    goto out;
            }
        } else {
            goto out;
        }
    }
    rc = 0;

out:
    free(buf);
    if (rc != 0)
        fprintf(stderr, "reader rejected %s\n", path);
    return rc;
}

/* --- Writer ------------------------------------------------------------ */

struct ids {
    uint64_t proc, thread, counter;
};

/* Each iteration emits four events, in this order:
 *   0: slice begin on the thread, name "compute"
 *   1: counter on the counter track, value i
 *   2: instant on the thread, name "tick"
 *   3: slice end on the thread
 */
static int write_trace(const char *path, enum pf_mode mode, int iters,
                       struct ids *out)
{
    unlink(path);

    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = path;
    cfg.format = PF_FORMAT_RAW;
    cfg.mode = mode;

    struct pf_trace *t = pf_trace_open(&cfg);
    if (!t)
        return -1;

    out->proc = pf_track_process(t, 42, "rawapp");
    out->thread = pf_track_thread(t, out->proc, 42, 1, "main");
    out->counter = pf_track_counter(t, out->proc, "depth", PF_UNIT_COUNT);

    for (int i = 0; i < iters; i++) {
        pf_slice_begin(t, out->thread, "compute");
        pf_counter_set_int(t, out->counter, i);
        pf_instant_event(t, out->thread, "tick");
        pf_slice_end(t, out->thread);
    }

    pf_trace_close(t);
    return 0;
}

/* --- Checks ------------------------------------------------------------ */

/* Compares event k to what was emitted at position k. Returns true if it
 * matches. */
static bool event_matches(const struct parsed *p, size_t k,
                          const struct ids *ids)
{
    const struct rev *e = &p->ev[k];
    int pos = (int)(k % 4);
    int i = (int)(k / 4);

    switch (pos) {
    case 0:
    case 2: {
        const char *want = pos == 0 ? "compute" : "tick";
        uint32_t want_type = pos == 0 ? TYPE_SLICE_BEGIN : TYPE_INSTANT;
        uint64_t want_track = ids->thread;
        const struct rname *n = find_name(p, e->u);
        return e->type == want_type && e->track == want_track &&
               e->flags == 0 && n && strcmp(n->s, want) == 0;
    }
    case 1:
        return e->type == TYPE_COUNTER && e->track == ids->counter &&
               e->flags == RF_HAS_VALUE && e->u == (uint64_t)i;
    default:
        return e->type == TYPE_SLICE_END && e->track == ids->thread &&
               e->flags == 0 && e->u == 0;
    }
}

static void check_contents(const struct parsed *p, const struct ids *ids,
                           size_t want_events)
{
    CHECK(p->version == 1, "version %u", p->version);
    CHECK(p->record_size == REC_BYTES, "record_size %u", p->record_size);
    CHECK(p->clock_id == (uint32_t)CLOCK_BOOTTIME, "clock_id %u", p->clock_id);
    CHECK(p->flags == 0, "flags %u", p->flags);

    /* Four tracks: the process track pf_trace_open() registers for getpid(),
     * then the three this test creates. */
    CHECK(p->ntrk == 4, "tracks %zu, want 4", p->ntrk);
    if (p->ntrk == 4) {
        CHECK(p->trk[0].kind == 0 && p->trk[0].pid == (int32_t)getpid(),
              "open's process track");
        CHECK(p->trk[1].uuid == ids->proc && p->trk[1].kind == 0 &&
                  p->trk[1].pid == 42 && strcmp(p->trk[1].name, "rawapp") == 0,
              "process track");
        CHECK(p->trk[2].uuid == ids->thread && p->trk[2].kind == 1 &&
                  p->trk[2].tid == 1 && strcmp(p->trk[2].name, "main") == 0,
              "thread track");
        CHECK(p->trk[3].uuid == ids->counter && p->trk[3].kind == 2 &&
                  p->trk[3].unit == PF_UNIT_COUNT &&
                  strcmp(p->trk[3].name, "depth") == 0,
              "counter track");
    }

    CHECK(p->nnm == 2, "names %zu, want 2", p->nnm);
    CHECK(p->nev <= want_events, "events %zu, want at most %zu", p->nev,
          want_events);

    for (size_t k = 0; k < p->nev; k++) {
        if (k > 0)
            CHECK(p->ev[k].ts >= p->ev[k - 1].ts, "ts went backwards at %zu",
                  k);
        if (!event_matches(p, k, ids)) {
            CHECK(false, "event %zu does not match what was emitted", k);
            break;
        }
    }
}

static void round_trip(const char *label, enum pf_mode mode, int iters,
                       const char *path)
{
    struct ids ids;
    if (write_trace(path, mode, iters, &ids) != 0) {
        CHECK(false, "%s: open failed: %s", label, strerror(errno));
        return;
    }

    struct parsed p;
    if (parse_raw(path, &p) != 0) {
        CHECK(false, "%s: reader rejected the file", label);
        return;
    }
    CHECK(p.torn_bytes == 0, "%s: %zu torn bytes in a closed file", label,
          p.torn_bytes);
    check_contents(&p, &ids, (size_t)iters * 4);
    CHECK(p.nev == (size_t)iters * 4, "%s: events %zu, want %d", label, p.nev,
          iters * 4);

    free(p.ev);
}

/* A torn tail must be reported and must leave a valid prefix behind. */
static void torn_tail(const char *path, int iters)
{
    struct ids ids;
    if (write_trace(path, PF_MODE_CONTINUOUS, iters, &ids) != 0) {
        CHECK(false, "torn: open failed");
        return;
    }

    /* Cut into the last chunk. */
    FILE *f = fopen(path, "rb");
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    CHECK(truncate(path, size - 5) == 0, "truncate failed");

    struct parsed p;
    if (parse_raw(path, &p) != 0) {
        CHECK(false, "torn: reader rejected a valid prefix");
        return;
    }
    CHECK(p.torn_bytes > 0, "torn: tail not detected");
    CHECK(p.nev < (size_t)iters * 4, "torn: no events lost");
    check_contents(&p, &ids, (size_t)iters * 4);
    free(p.ev);
}

/* A raw file must not be opened on top of existing contents. */
static void refuses_existing(const char *path)
{
    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = path;
    cfg.format = PF_FORMAT_RAW;

    errno = 0;
    struct pf_trace *t = pf_trace_open(&cfg);
    CHECK(t == NULL && errno == EEXIST, "reopen: expected EEXIST, got %d",
          errno);
    if (t)
        pf_trace_close(t);
}

/* --- Simple API -------------------------------------------------------- */

#define SIMPLE_THREADS 4
#define SIMPLE_ROUNDS 10

struct simple_arg {
    int id;
    pid_t tid;
};

static void *simple_worker(void *arg)
{
    struct simple_arg *a = arg;
    char name[16];
    snprintf(name, sizeof(name), "worker-%d", a->id);
    pf_thread_name(name);
    a->tid = gettid();

    for (int i = 0; i < SIMPLE_ROUNDS; i++) {
        pf_slice_begin_sp("job");
        pf_instant_event_sp("tick");
        pf_slice_end_sp();
    }
    return NULL;
}

static const struct rtrack *find_thread_track(const struct parsed *p, pid_t tid)
{
    for (size_t i = 0; i < p->ntrk; i++)
        if (p->trk[i].kind == 1 && p->trk[i].tid == (int32_t)tid)
            return &p->trk[i];
    return NULL;
}

/* Each thread's first _sp call registers its own track, and the name set by
 * pf_thread_name() before that call is what the track carries. Every event
 * a thread emits must sit on its own track and nothing else's. */
static void simple_threads(const char *path)
{
    unlink(path);
    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = path;
    cfg.format = PF_FORMAT_RAW;
    cfg.mode = PF_MODE_SNAPSHOT;

    struct pf_trace *t = pf_trace_open(&cfg);
    if (!t) {
        CHECK(false, "simple threads: open failed: %s", strerror(errno));
        return;
    }

    pthread_t th[SIMPLE_THREADS];
    struct simple_arg args[SIMPLE_THREADS];
    for (int i = 0; i < SIMPLE_THREADS; i++) {
        args[i] = (struct simple_arg){ .id = i };
        CHECK(pthread_create(&th[i], NULL, simple_worker, &args[i]) == 0,
              "simple threads: pthread_create %d", i);
    }
    for (int i = 0; i < SIMPLE_THREADS; i++)
        pthread_join(th[i], NULL);
    pf_trace_close(t);

    struct parsed p;
    if (parse_raw(path, &p) != 0) {
        CHECK(false, "simple threads: reader rejected the file");
        return;
    }
    CHECK(p.torn_bytes == 0, "simple threads: %zu torn bytes", p.torn_bytes);
    /* One more than SIMPLE_THREADS: the process track from pf_trace_open(). */
    CHECK(p.ntrk == SIMPLE_THREADS + 1, "simple threads: tracks %zu, want %d",
          p.ntrk, SIMPLE_THREADS + 1);

    for (int i = 0; i < SIMPLE_THREADS; i++) {
        char want[16];
        snprintf(want, sizeof(want), "worker-%d", i);
        const struct rtrack *tr = find_thread_track(&p, args[i].tid);
        if (!tr) {
            CHECK(false, "simple threads: no track for worker %d", i);
            continue;
        }
        CHECK(strcmp(tr->name, want) == 0,
              "simple threads: track for %s is named '%s'", want, tr->name);

        size_t begins = 0, ticks = 0, ends = 0;
        for (size_t k = 0; k < p.nev; k++) {
            const struct rev *e = &p.ev[k];
            if (e->track != tr->uuid)
                continue;
            if (e->type == TYPE_SLICE_BEGIN)
                begins++;
            else if (e->type == TYPE_INSTANT)
                ticks++;
            else if (e->type == TYPE_SLICE_END)
                ends++;
        }
        CHECK(begins == SIMPLE_ROUNDS && ticks == SIMPLE_ROUNDS &&
                  ends == SIMPLE_ROUNDS,
              "simple threads: %s has %zu/%zu/%zu begin/tick/end, want %d each",
              want, begins, ticks, ends, SIMPLE_ROUNDS);
    }

    free(p.ev);
}

/* A name set after the track exists must reach the file. The track is
 * re-declared under the same uuid, so the reader must accept the file and
 * show the new name on the track the events went to. */
static void simple_rename(const char *path)
{
    unlink(path);
    struct pf_config cfg;
    pf_config_init(&cfg);
    cfg.path = path;
    cfg.format = PF_FORMAT_RAW;
    cfg.mode = PF_MODE_CONTINUOUS;

    struct pf_trace *t = pf_trace_open(&cfg);
    if (!t) {
        CHECK(false, "simple rename: open failed: %s", strerror(errno));
        return;
    }

    /* The flush makes the first declaration reach the file before the
     * rename, which is the case that needs the track re-declared. */
    pf_slice_begin_sp("before");
    pf_slice_end_sp();
    CHECK(pf_trace_flush(t) == 0, "simple rename: flush failed");
    pf_thread_name("renamed");
    pf_slice_begin_sp("after");
    pf_slice_end_sp();
    pf_trace_close(t);

    struct parsed p;
    if (parse_raw(path, &p) != 0) {
        CHECK(false, "simple rename: reader rejected the file");
        return;
    }
    const struct rtrack *tr = find_thread_track(&p, gettid());
    if (!tr) {
        CHECK(false, "simple rename: no thread track for this thread");
    } else {
        CHECK(strcmp(tr->name, "renamed") == 0,
              "simple rename: track named '%s', want 'renamed'", tr->name);
        size_t on_track = 0;
        for (size_t k = 0; k < p.nev; k++)
            if (p.ev[k].track == tr->uuid)
                on_track++;
        CHECK(on_track == 4, "simple rename: %zu events on track, want 4",
              on_track);
    }
    free(p.ev);
}

int main(void)
{
    /* Continuous: the drain writes several event chunks, since 5000
     * iterations are 20000 records against a 128 KiB flush threshold. */
    round_trip("continuous", PF_MODE_CONTINUOUS, 5000,
               "raw_roundtrip_continuous.pfraw");

    /* Snapshot: a single dump on close, one events chunk. */
    round_trip("snapshot", PF_MODE_SNAPSHOT, 1000,
               "raw_roundtrip_snapshot.pfraw");

    torn_tail("raw_roundtrip_torn.pfraw", 2000);
    refuses_existing("raw_roundtrip_continuous.pfraw");

    simple_threads("raw_roundtrip_simple_threads.pfraw");
    simple_rename("raw_roundtrip_simple_rename.pfraw");

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("raw round trip: all checks passed\n");
    return 0;
}
