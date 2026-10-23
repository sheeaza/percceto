#ifndef PFTRACE_INTERNAL_H
#define PFTRACE_INTERNAL_H

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
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
 * The head one event occupies in a chunk, written by the owning producer
 * alone. The head is fixed size. A dynamic name is copied after it, so the
 * record's size is PF_REC_HEAD_BYTES, plus the copied name padded to
 * PF_REC_ALIGN when RF_NAME_COPIED is set. See docs/raw_format.md.
 *
 * A static name is a borrowed pointer: names are string literals, so copying
 * them would spend bytes and a memcpy per event to duplicate something
 * immortal. The drain dereferences the pointer and interns it.
 *
 * @name and @value share eight bytes because no event carries both. A slice
 * or instant has a name and no value; a counter has a value and no name,
 * which is Perfetto's own rule -- "most other TrackEvent fields (e.g.
 * categories, name, ..) are not supported for TYPE_COUNTER events".
 * RF_HAS_VALUE selects the arm. With RF_NAME_COPIED, the same eight bytes
 * hold the copied name's length instead.
 *
 *   0       8       16      24   28
 *   +-------+-------+-------+----+----+
 *   | ts    | track | name  |type|flag|
 *   |       |       | value |    |    |
 *   +-------+-------+-------+----+----+
 *
 * Counters never carry a name, copied or not. */
#define PF_REC_HEAD_BYTES 32u
#define PF_REC_ALIGN 8u
/* The argument section: a u32 count and 4 reserved bytes, then one entry of
 * PF_REC_ARG_BYTES per argument. See docs/raw_format.md. */
#define PF_REC_ARGS_HEAD 8u
#define PF_REC_ARG_BYTES 24u

/* Longest dynamic name stored, in bytes, not counting the NUL. A longer name
 * is cut to this length, the last three bytes become "...", and the cut is
 * counted. The reader sees the "..." and knows the name was cut. */
#define PF_DYN_NAME_MAX 128u

/* The length a dynamic name is stored at: its own, or PF_DYN_NAME_MAX when it
 * is longer. @cut is set when it is longer. */
static inline size_t pf_dyn_name_fit(const char *name, bool *cut)
{
    size_t n = strlen(name);
    *cut = n > PF_DYN_NAME_MAX;
    return *cut ? PF_DYN_NAME_MAX : n;
}

/* Copies @n bytes of @name to @dst, which is @n bytes long. When @cut, the last
 * three bytes are "..." in place of the cut text. */
static inline void pf_dyn_name_copy(char *dst, const char *name, size_t n, bool cut)
{
    if (!cut) {
        memcpy(dst, name, n);
        return;
    }
    memcpy(dst, name, n - 3);
    memcpy(dst + n - 3, "...", 3);
}

struct pf_rec {
    uint64_t ts; /* timestamp, ns */
    uint64_t track; /* track_uuid */

    /* Borrowed string literal, or a counter's value; see RF_HAS_VALUE. With
     * RF_NAME_COPIED, the length of the name copied after the head. */
    union {
        const char *name;
        uint64_t value;
        uint64_t name_len;
    } u;

    uint32_t type; /* TYPE_* */
    uint32_t flags; /* see RF_* */
};

enum pf_rec_flag {
    RF_HAS_VALUE = 0x1u,
    RF_VALUE_IS_DOUBLE = 0x2u,
    /* The name's bytes follow the head, padded to PF_REC_ALIGN. */
    RF_NAME_COPIED = 0x4u,
    /* An argument section follows the head, before any copied name. See
     * pf_rec_args_bytes(). */
    RF_HAS_ARGS = 0x8u
};

/* The layout is the on-chunk wire format, so pin it. */
_Static_assert(sizeof(struct pf_rec) == PF_REC_HEAD_BYTES, "the head is exactly PF_REC_HEAD_BYTES");
_Static_assert(offsetof(struct pf_rec, ts) == 0, "ts leads the record");
_Static_assert(offsetof(struct pf_rec, track) == 8, "track follows ts");
_Static_assert(offsetof(struct pf_rec, u) == 16, "name/value follows track");
_Static_assert(offsetof(struct pf_rec, type) == 24, "type follows name/value");
_Static_assert(offsetof(struct pf_rec, flags) == 28, "no padding before flags");

/* --- Chunks and writers -------------------------------------------------
 * A chunk is one thread's run of records, plus the link that puts it on the
 * committed FIFO or the free list. It is written by its owning producer until
 * it is committed, and read by the drain only after that. See
 * docs/chunk_buffers_design.md. */
struct pf_chunk {
    struct pf_chunk *next; /* committed FIFO or free list */
    /* Bytes of @bytes published to the drain. The owner stores it after each
     * record, so a snapshot dump can read a chunk that is still being filled. */
    _Atomic uint32_t used;
    uint32_t writer; /* owning writer slot */
    uint64_t seq; /* per-writer chunk number */
    /* Records back to back: a head of PF_REC_HEAD_BYTES, plus a copied name.
     * Records never split across chunks: one that does not fit the rest
     * rotates to the next chunk. */
    _Alignas(8) unsigned char bytes[];
};

/* One interned name in a writer's cache: the literal's address, and the epoch
 * of the chunk its definition was last written into. */
#define PF_PB_INTERN_SLOTS 32

struct pb_intern_slot {
    const char *key; /* NULL until first used */
    size_t len; /* strlen(key), so a hit does not measure the name again */
    uint32_t epoch;
};

/* One per bound thread. The producer owns @cur, @used and @seq. @bound is
 * read and written only under reg_lock. Aligned to a cache line so two
 * producers' hot fields never share one; the array is allocated to match. */
struct pf_writer {
    _Alignas(64) struct pf_chunk *cur; /* NULL until the first rotation */
    uint32_t used; /* bytes in @cur; a record that would pass chunk_payload rotates */
    uint32_t id;
    uint64_t seq;
    /* Bumped on every rotation. A name's definition is in the current chunk
     * only if its cache slot was stamped with this epoch. */
    uint32_t epoch;
    /* Protobuf: this writer's interned names. Slot i is iid i + 1 in the
     * writer's own sequence, so iids never collide across writers. */
    struct pb_intern_slot intern[PF_PB_INTERN_SLOTS];
    /* Protobuf: this writer's argument keys, kept apart from event names: they
     * are sent as debug_annotation_names, a separate iid space. */
    struct pb_intern_slot keys[PF_PB_INTERN_SLOTS];
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
#define PF_PACKET_WARM (PF_REC_HEAD_BYTES + PF_NAME_WARM)

/* Longest name an event with args may take once its arguments are rendered,
 * including the NUL. A longer result falls back to the bare format string. */

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
    /* Position in every thread's pf_tls table; see pf_index_acquire. */
    uint32_t index;
    /* Directory the output goes in, held for the life of the trace. */
    int dirfd;
    /* Continuous: the one output file, held open. Snapshot: -1 except while a
     * dump is writing its file. */
    int fd;
    struct pf_config cfg;

    /* The owner's reference, plus one per thread bound to this trace. The
     * last to drop one finishes the trace; see pf_trace_unref. */
    _Atomic uint32_t refs;

    /* --- Chunk pool, guarded by pool_lock ---------------------------------
     * One allocation at open holds every chunk. A chunk is on exactly one of
     * the free list, a writer's @cur, or the committed FIFO. */
    void *pool_mem;
    size_t chunk_bytes;
    uint32_t chunk_count;
    uint32_t chunk_slots;
    /* Record bytes a chunk holds: chunk_slots records. A writer's @used
     * reaches this and rotates. */
    uint32_t chunk_payload;
    struct pf_chunk *free_list;
    struct pf_chunk *committed_head;
    struct pf_chunk *committed_tail;
    uint32_t committed_count;
    /* Continuous mode: committed chunks at which a producer wakes the drain
     * thread, from cfg.watermark_pct. */
    uint32_t watermark_chunks;
    pthread_mutex_t pool_lock;

    /* Writer slots. reg_lock guards the bound flags only. */
    struct pf_writer *writers;
    pthread_mutex_t reg_lock;

    /* Records lost to a full pool in continuous mode. Atomic because the
     * producer that loses an event does not take pool_lock. */
    _Atomic uint64_t dropped;
    _Atomic uint64_t truncated;

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
    /* Argument keys, for raw. Kept apart from names: a key is sent in its own
     * KEYS chunk, and its pointer is only ever a key. */
    struct intern_table key_table;
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
     * as a chunk, in the order tracks, names, keys, then events, before the
     * events that reference them. Only allocated when the trace's format is
     * raw. */
    struct {
        struct pb_buf names;
        struct pb_buf keys;
        struct pb_buf tracks;
        struct pb_buf events;
    } raw;

    _Atomic uint64_t bytes_written;
};

/* --- Chunk pool and writers, defined in pf_chunk.c ---------------------- */

/* Allocates the pool and writer table. The pool is the trace's only sizeable
 * allocation. Returns 0, or -1 with errno set. */
size_t pf_chunk_bytes(uint32_t slots);
int pf_pool_init(struct pf_trace *t);
void pf_pool_destroy(struct pf_trace *t);

/* Binds the calling thread to @t and takes a reference to it, released by
 * pf_unbind_thread. Returns NULL when every slot is taken; the thread is still
 * bound, with no slot, and its miss is cached in its pf_tls entry. Called only
 * when pf_writer_for misses. */
struct pf_writer *pf_writer_bind(struct pf_trace *t);

/* Commits the calling thread's partial chunk for @t, frees its slot, and drops
 * its reference to @t. Does nothing if the thread is not bound to @t. */
void pf_unbind_thread(struct pf_trace *t);

/* Claims a trace index, the position of the trace in every thread's pf_tls.
 * Returns -1 with ENOSPC when PF_MAX_TRACES traces are open. */
int pf_index_acquire(struct pf_trace *t);
void pf_index_release(struct pf_trace *t);

/* Drops one reference to @t. The last one finishes the trace: stops the drain,
 * writes what is buffered, and frees everything. Defined in pftrace.c. */
void pf_trace_unref(struct pf_trace *t);

/* Commits @w's full chunk and takes a new one. Returns false if no chunk is
 * free, in which case the caller drops the event. Slow path: once per chunk. */
bool pf_writer_rotate(struct pf_trace *t, struct pf_writer *w);

/* Continuous drain: detaches the committed FIFO and returns its head. The
 * drain writes it, then gives it back with pf_chunks_release. */
struct pf_chunk *pf_committed_take(struct pf_trace *t);
void pf_chunks_release(struct pf_trace *t, struct pf_chunk *head);

/* Continuous drain: commits the calling thread's partial chunk, so the drain
 * includes its last events. Caller holds pool_lock. */
void pf_writer_flush_own_locked(struct pf_trace *t);

/* Snapshot drain: the committed FIFO stays in place while the lock is held,
 * so producers that need a chunk wait until the dump is done. Every writer's
 * partial chunk is linked after the FIFO, so one walk covers all of them. It
 * is still being filled, so only its published records are walked. Caller
 * holds pool_lock; pf_unlink_partials_locked must follow the walk. */
struct pf_chunk *pf_link_partials_locked(struct pf_trace *t);
void pf_unlink_partials_locked(struct pf_trace *t);

/* Total records emitted by every writer. Takes reg_lock. */
uint64_t pf_writers_events(struct pf_trace *t);

/* Wakes the drain thread once committed chunks pass the watermark. Defined
 * in pftrace.c. Continuous mode only. */
void pf_drain_wake(struct pf_trace *t);

/* Per-thread writer table. One pf_tls per thread, for the whole process: each
 * trace has an index, and the thread's slot at that index holds the trace and
 * its writer. A slot is set only while the thread holds a reference to the
 * trace, so the trace cannot finish, and its index cannot be reused, while the
 * slot points at it. Overflow threads keep a slot with a NULL writer, so their
 * miss is cached too. */
#define PF_MAX_TRACES 8

struct pf_tls_slot {
    struct pf_trace *trace;
    struct pf_writer *writer;
    /* The thread's track in @trace, for the simple API. 0 until registered.
     * Cleared with the slot, so a rebind registers again. */
    uint64_t track_uuid;
};

/* Per-thread state that is not tied to one trace. */
struct pf_thread_state {
    struct pf_tls_slot slots[PF_MAX_TRACES];
    /* Name set by pf_thread_name, applied when the thread's track is
     * registered in a trace. */
    bool has_name;
    char name[PF_TRACK_NAME_MAX];
};

extern __thread struct pf_thread_state pf_tls;

static inline struct pf_writer *pf_writer_for(struct pf_trace *t)
{
    struct pf_tls_slot *s = &pf_tls.slots[t->index];
    if (s->trace == t)
        return s->writer;
    return pf_writer_bind(t);
}

/* --- Walking committed chunks ------------------------------------------
 * A walk visits every record of a list of committed chunks, in commit order.
 * Records within one chunk are in order, and a thread's chunks are in order
 * because they commit in sequence. The walk is not sorted across threads;
 * Perfetto sorts by timestamp on import. */
struct pf_walk {
    const struct pf_chunk *c;
    /* Byte offset into @c->bytes of the next record. */
    uint32_t i;
    /* Bytes of @c to walk, read once when the walk enters the chunk. A live
     * chunk's count can rise during the walk, and bytes below the count it
     * was read with are never rewritten before the chunk is freed. */
    uint32_t end;
};

static inline void pf_walk_enter(struct pf_walk *w, const struct pf_chunk *c)
{
    w->c = c;
    w->i = 0;
    w->end = c ? atomic_load_explicit(&c->used, memory_order_acquire) : 0;
}

static inline void pf_walk_begin(struct pf_walk *w, const struct pf_chunk *head)
{
    pf_walk_enter(w, head);
}

/* A record's size in its chunk: the head, the argument section if any, and any
 * copied name padded to PF_REC_ALIGN. Read from the head, so the walk needs no
 * index. */
static inline uint32_t pf_rec_args_bytes(const struct pf_rec *r)
{
    if (!(r->flags & RF_HAS_ARGS))
        return 0;
    uint32_t n = *(const uint32_t *)((const char *)r + PF_REC_HEAD_BYTES);
    return PF_REC_ARGS_HEAD + n * PF_REC_ARG_BYTES;
}

static inline uint32_t pf_rec_size(const struct pf_rec *r)
{
    uint32_t size = PF_REC_HEAD_BYTES + pf_rec_args_bytes(r);
    if (!(r->flags & RF_NAME_COPIED))
        return size;
    uint32_t len = (uint32_t)r->u.name_len;
    return size + ((len + PF_REC_ALIGN - 1) & ~(PF_REC_ALIGN - 1));
}

static inline const struct pf_rec *pf_walk_next(struct pf_walk *w)
{
    while (w->c && w->i >= w->end)
        pf_walk_enter(w, w->c->next);
    if (!w->c)
        return NULL;
    const struct pf_rec *r = (const struct pf_rec *)(w->c->bytes + w->i);
    w->i += pf_rec_size(r);
    return r;
}

/* --- Shared helpers, defined in pftrace.c ------------------------------ */

static inline uint64_t now_ns(clockid_t clk)
{
    struct timespec ts;
    clock_gettime(clk, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int out_flush(struct pf_trace *t);
bool intern_lookup_or_insert(struct intern_table *tab, const char *key, uint64_t *iid_out,
                             bool *is_new_out);
void intern_table_reset(struct intern_table *tab);

/* Copies track @i into @out if its descriptor is due: it was registered at
 * or after index @first, or it was renamed since it was last emitted. On
 * return true, the copy is marked emitted. Takes track_lock, so the caller
 * must not hold it. Caller holds out_lock. */
bool track_emit_due(struct pf_trace *t, unsigned i, unsigned first, struct track_def *out);

/* --- The two output formats ------------------------------------------- */

/* Each writes the records of @head, in commit order. Called from drain_locked
 * with out_lock held. */
int pb_drain_locked(struct pf_trace *t, const struct pf_chunk *head);
int raw_drain_locked(struct pf_trace *t, const struct pf_chunk *head);
int raw_write_header(struct pf_trace *t); /* pftrace_raw.c */

#endif /* PFTRACE_INTERNAL_H */
