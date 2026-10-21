#ifndef PFTRACE_INTERNAL_H
#define PFTRACE_INTERNAL_H

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "pb_writer.h"
#include "pftrace.h"

/* Declarations shared by pftrace.c, pftrace_pb.c, pftrace_raw.c and
 * pf_chunk.c. Not public API; see include/pftrace.h for that. */

/* Event type values, as the chunks store them. Shared by emit_event and
 * both output formats. */
#define TYPE_SLICE_BEGIN 1
#define TYPE_SLICE_END 2
#define TYPE_INSTANT 3
#define TYPE_COUNTER 4

/* --- Record layout ------------------------------------------------------
 * The payload one event occupies in a chunk, written by the owning
 * producer alone. Fixed size, so a chunk is an array of these.
 *
 * The name is a borrowed pointer rather than inline bytes: names are string
 * literals, so copying them would spend up to 255 bytes and a memcpy per
 * event to duplicate something immortal. The drain dereferences the pointer
 * and interns it, which keeps the strlen and the hashing off the hot path.
 *
 * @name and @value share eight bytes because no event carries both. A slice
 * or instant has a name and no value; a counter has a value and no name,
 * which is Perfetto's own rule -- "most other TrackEvent fields (e.g.
 * categories, name, ..) are not supported for TYPE_COUNTER events".
 * RF_HAS_VALUE selects the arm.
 *
 *   0       8       16      24   28   32      40      48
 *   +-------+-------+-------+----+----+-------+-------+
 *   | ts    | track | name  |type|flag| arg0  | arg1  |
 *   |       |       | value |    |    |       |       |
 *   +-------+-------+-------+----+----+-------+-------+
 *
 * arg0 and arg1 hold up to PF_MAX_ARGS event arguments, as int64 bits or
 * double bits. RF_NARGS_MASK gives how many are used and the RF_ARG*_IS_DOUBLE
 * bits give each one's type. They are zeroed on events with no args, so a
 * chunk never carries stale bytes. Counters never carry args. */
#define PF_PAYLOAD_BYTES 48u
#define PF_REC_ALIGN 8u

struct pf_rec {
    uint64_t ts; /* timestamp, ns */
    uint64_t track; /* track_uuid */

    /* Borrowed string literal, or a counter's value; see RF_HAS_VALUE. For
     * an event with args, the name is the format string. */
    union {
        const char *name;
        uint64_t value;
    } u;

    uint32_t type; /* TYPE_* */
    uint32_t flags; /* see RF_* */

    uint64_t arg[PF_MAX_ARGS];
};

enum pf_rec_flag {
    RF_HAS_VALUE = 0x1u,
    RF_VALUE_IS_DOUBLE = 0x2u,
    /* Argument count, 0 to PF_MAX_ARGS, in bits 2-3. */
    RF_NARGS_SHIFT = 2,
    RF_NARGS_MASK = 0x3u << 2,
    RF_ARG0_IS_DOUBLE = 0x10u,
    RF_ARG1_IS_DOUBLE = 0x20u
};

/* The layout is the on-chunk wire format, so pin it. */
_Static_assert(sizeof(struct pf_rec) == PF_PAYLOAD_BYTES,
               "the record is exactly PF_PAYLOAD_BYTES");
_Static_assert(offsetof(struct pf_rec, ts) == 0, "ts leads the record");
_Static_assert(offsetof(struct pf_rec, track) == 8, "track follows ts");
_Static_assert(offsetof(struct pf_rec, u) == 16, "name/value follows track");
_Static_assert(offsetof(struct pf_rec, type) == 24, "type follows name/value");
_Static_assert(offsetof(struct pf_rec, flags) == 28, "no padding before flags");
_Static_assert(offsetof(struct pf_rec, arg) == 32, "args follow the header");
_Static_assert(PF_MAX_ARGS <= 3, "RF_NARGS_MASK holds counts up to 3");

/* --- Chunks and writers -------------------------------------------------
 * A chunk is one thread's run of records, plus the link that puts it on the
 * committed FIFO or the free list. It is written by its owning producer until
 * it is committed, and read by the drain only after that. See
 * docs/chunk_buffers_design.md. */
struct pf_chunk {
    struct pf_chunk *next; /* committed FIFO or free list */
    uint32_t used; /* records written; set at commit */
    uint32_t writer; /* owning writer slot */
    uint64_t seq; /* per-writer chunk number */
    struct pf_rec recs[]; /* cfg.chunk_slots records */
};

/* One per bound thread. The producer owns @cur, @used and @seq. @bound is
 * read and written only under reg_lock. Aligned to a cache line so two
 * producers' hot fields never share one; the array is allocated to match. */
struct pf_writer {
    _Alignas(64) struct pf_chunk *cur; /* NULL until the first rotation */
    uint32_t used; /* records in @cur; chunk_slots forces a rotation */
    uint32_t id;
    uint64_t seq;
    /* Records this writer has emitted into its chunks, including any that a
     * snapshot later overwrites. Written only by the owner, read by stats. */
    _Atomic uint64_t events;
    bool bound;
};

/* Cap on a track's stored name. Fixed so the track table is one
 * allocation at open and never grows. */
#define PF_TRACK_NAME_MAX 64

/* How much to presize the staging buffers for. These are hints, not limits:
 * an event name is a borrowed literal of any length, so pb_buf grows if one
 * exceeds this. */
#define PF_NAME_WARM 256u
#define PF_PACKET_WARM (PF_PAYLOAD_BYTES + PF_NAME_WARM)

/* Longest name an event with args may take once its arguments are rendered,
 * including the NUL. A longer result falls back to the bare format string. */
#define PF_FORMAT_NAME_MAX 128u

/* Flush the staging buffer once it passes this, so one write() carries many
 * packets instead of one syscall per event. */
#define PF_OUT_FLUSH_THRESHOLD (128u * 1024u)

/* Interning table storage. The operations are in pftrace.c. */
#define PF_INTERN_CAP 1021 /* prime */

struct intern_slot {
    const char *key; /* NULL = free slot */
    uint64_t iid;
};

struct intern_table {
    struct intern_slot slots[PF_INTERN_CAP];
    size_t count;
    uint64_t next_iid;
};

/* --- Track table --------------------------------------------------------
 * Track descriptors cannot live in the chunks: they are dropped as the
 * window moves on, so a descriptor emitted at startup would be gone by the
 * time a dump happened, leaving events referring to unknown track_uuids.
 * They are kept here instead and re-emitted whenever a dump needs them. */

enum track_kind {
    TRACK_PROCESS,
    TRACK_THREAD,
    TRACK_COUNTER,
};

struct track_def {
    enum track_kind kind;
    uint64_t uuid;
    uint64_t parent_uuid;
    int32_t pid;
    int32_t tid;
    enum pf_unit unit;
    bool has_name;
    char name[PF_TRACK_NAME_MAX];
    /* Bumped on every rename. A track is re-emitted when its name_version
     * differs from emitted_version, which is how a rename reaches the
     * output after the descriptor has already been written. */
    uint32_t name_version;
    uint32_t emitted_version;
};

/* --- Trace context ---------------------------------------------------- */

struct pf_trace {
    /* Directory the output goes in, held for the life of the trace. */
    int dirfd;
    /* Continuous: the one output file, held open. Snapshot: -1 except while a
     * dump is writing its file. */
    int fd;
    struct pf_config cfg;

    /* Unique per open. A thread's cached writer is valid only if both its
     * trace pointer and this generation match, so a freed-and-reused
     * trace address never picks up a stale writer. */
    uint64_t gen;

    /* --- Chunk pool, guarded by pool_lock ---------------------------------
     * One allocation at open holds every chunk. A chunk is on exactly one of
     * the free list, a writer's @cur, or the committed FIFO. */
    void *pool_mem;
    size_t chunk_bytes;
    uint32_t chunk_count;
    uint32_t chunk_slots;
    struct pf_chunk *free_list;
    struct pf_chunk *committed_head;
    struct pf_chunk *committed_tail;
    uint32_t committed_count;
    /* Continuous mode: committed chunks at which a producer wakes the drain
     * thread, from cfg.watermark_pct. */
    uint32_t watermark_chunks;
    pthread_mutex_t pool_lock;

    /* Link on the open-trace registry; see pf_registry_add. */
    struct pf_trace *open_next;

    /* Writer slots. reg_lock guards the bound flags only. */
    struct pf_writer *writers;
    pthread_mutex_t reg_lock;

    /* Records lost to a full pool in continuous mode. Atomic because the
     * producer that loses an event does not take pool_lock. */
    _Atomic uint64_t dropped;

    /* Serializes drains: a caller's pf_trace_dump() against the drain
     * thread, and against another caller's. Also covers the intern tables,
     * the staging buffers, tracks_emitted, fd and dump_seq. */
    pthread_mutex_t out_lock;

    /* Guards the track table against concurrent pf_track_* calls, and
     * against a drain reading it. Not a hot-path lock. */
    pthread_mutex_t track_lock;
    struct track_def *tracks;
    unsigned track_count;
    unsigned tracks_emitted;
    uint64_t next_uuid;

    /* Snapshot mode: number of the next dump, used in its file name. Per
     * trace, so it only separates dumps made within one run. */
    uint32_t dump_seq;
    /* Continuous mode: the name of the open file, copied at open so a dump
     * can report it. */
    char file_name[NAME_MAX + 1];

    /* Uuid of the process track for getpid(), registered at open. The parent
     * of the thread tracks the simple API creates. 0 if the table had no
     * room for it. */
    uint64_t process_uuid;

    /* Drain thread wakeup. wake_pending is checked by producers without the
     * lock, so one futex is spent per drain cycle rather than per commit
     * past the watermark. */
    pthread_t drain_thread; /* valid only in PF_MODE_CONTINUOUS */
    pthread_mutex_t drain_lock;
    pthread_cond_t drain_cv;
    _Atomic unsigned wake_pending;
    bool stop;

    /* Reused across dumps so steady-state draining allocates nothing. The
     * output buffer is shared by both formats. */
    struct pb_buf out;
    struct intern_table name_table;
    bool header_written;

    /* Staging for the protobuf path. Only allocated when the trace's format
     * is protobuf. */
    struct {
        struct pb_buf scratch_a;
        struct pb_buf scratch_b;
        struct pb_buf scratch_c;
        /* Holds one InternedData entry while it is built, before being
         * embedded into scratch_a. Separate so the two never alias. */
        struct pb_buf scratch_entry;
    } pb;

    /* Staging for the raw path, one buffer per chunk kind. Each is emitted
     * as a chunk, in the order tracks, names, then events, before the events
     * that reference them. Only allocated when the trace's format is raw. */
    struct {
        struct pb_buf names;
        struct pb_buf tracks;
        struct pb_buf events;
    } raw;

    _Atomic uint64_t bytes_written;
};

/* --- Chunk pool and writers, defined in pf_chunk.c ---------------------- */

/* Allocates the pool and writer table. The pool is the trace's only sizeable
 * allocation. Returns 0, or -1 with errno set. */
int pf_pool_init(struct pf_trace *t);
void pf_pool_destroy(struct pf_trace *t);

/* Binds the calling thread to a writer slot. Returns NULL when every slot
 * is taken, and that answer is cached for the thread. */
struct pf_writer *pf_writer_bind(struct pf_trace *t);

/* Commits @w's full chunk and takes a new one. Returns false if no chunk is
 * free, in which case the caller drops the event. Slow path: once per chunk. */
bool pf_writer_rotate(struct pf_trace *t, struct pf_writer *w);

/* Close only: commits every writer's partial chunk. The caller guarantees no
 * producer is emitting. */
void pf_writers_commit_all(struct pf_trace *t);

/* Continuous drain: detaches the committed FIFO and returns its head. The
 * drain writes it, then gives it back with pf_chunks_release. */
struct pf_chunk *pf_committed_take(struct pf_trace *t);
void pf_chunks_release(struct pf_trace *t, struct pf_chunk *head);

/* Snapshot drain: the committed FIFO stays in place while the lock is held,
 * so producers that need a chunk wait until the dump is done. */
/* Commits the calling thread's partial chunk, so a dump includes its last
 * events. Other threads' partial chunks are not touched. Caller holds
 * pool_lock. */
void pf_writer_flush_own_locked(struct pf_trace *t);

/* Open-trace registry. A trace is added at open and removed before close frees
 * anything. */
void pf_registry_add(struct pf_trace *t);
void pf_registry_remove(struct pf_trace *t);

/* Total records emitted by every writer. Takes reg_lock. */
uint64_t pf_writers_events(struct pf_trace *t);

/* Wakes the drain thread once committed chunks pass the watermark. Defined
 * in pftrace.c. Continuous mode only. */
void pf_drain_wake(struct pf_trace *t);

/* Hot path lookup: the calling thread's writer, cached in TLS. Binds it on
 * first use. */
struct pf_tls_cache {
    struct pf_trace *trace;
    uint64_t gen;
    struct pf_writer *writer;
};
extern __thread struct pf_tls_cache pf_tls;

static inline struct pf_writer *pf_writer_for(struct pf_trace *t)
{
    if (pf_tls.trace == t && pf_tls.gen == t->gen)
        return pf_tls.writer;
    return pf_writer_bind(t);
}

/* --- Walking committed chunks ------------------------------------------
 * A walk visits every record of a list of committed chunks, in commit order.
 * Records within one chunk are in order, and a thread's chunks are in order
 * because they commit in sequence. The walk is not sorted across threads;
 * Perfetto sorts by timestamp on import. */
struct pf_walk {
    const struct pf_chunk *c;
    uint32_t i;
};

static inline void pf_walk_begin(struct pf_walk *w, const struct pf_chunk *head)
{
    w->c = head;
    w->i = 0;
}

static inline const struct pf_rec *pf_walk_next(struct pf_walk *w)
{
    while (w->c && w->i >= w->c->used) {
        w->c = w->c->next;
        w->i = 0;
    }
    if (!w->c)
        return NULL;
    return &w->c->recs[w->i++];
}

/* --- Shared helpers, defined in pftrace.c ------------------------------ */

static inline uint64_t now_ns(clockid_t clk)
{
    struct timespec ts;
    clock_gettime(clk, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int out_flush(struct pf_trace *t);
bool intern_lookup_or_insert(struct intern_table *tab, const char *key,
                             uint64_t *iid_out, bool *is_new_out);
void intern_table_reset(struct intern_table *tab);

/* Copies track @i into @out if its descriptor is due: it was registered at
 * or after index @first, or it was renamed since it was last emitted. On
 * return true, the copy is marked emitted. Takes track_lock, so the caller
 * must not hold it. Caller holds out_lock. */
bool track_emit_due(struct pf_trace *t, unsigned i, unsigned first,
                    struct track_def *out);

/* --- The two output formats ------------------------------------------- */

/* Each writes the records of @head, in commit order. Called from drain_locked
 * with out_lock held. */
int pb_drain_locked(struct pf_trace *t, const struct pf_chunk *head);
int raw_drain_locked(struct pf_trace *t, const struct pf_chunk *head);
int raw_write_header(struct pf_trace *t); /* pftrace_raw.c */

#endif /* PFTRACE_INTERNAL_H */
