#ifndef PFTRACE_INTERNAL_H
#define PFTRACE_INTERNAL_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pb_writer.h"
#include "pf_ring.h"
#include "pftrace.h"

/* Declarations shared by pftrace.c, pftrace_pb.c and pftrace_raw.c. Not
 * public API; see include/pftrace.h for that. */

/* Event type values, as the ring stores them. Shared by emit_event and
 * both output formats. */
#define TYPE_SLICE_BEGIN 1
#define TYPE_SLICE_END 2
#define TYPE_INSTANT 3
#define TYPE_COUNTER 4

/* --- Ring record layout -------------------------------------------------
 * The payload one event occupies in the ring, written by the owning
 * producer alone. Whatever a backend needs for its own publication
 * bookkeeping sits outside this -- pf_ring_reserve hands back a pointer to
 * the payload, so this struct is the same shape on every backend and the
 * comparison between them is not measuring a difference in record size.
 *
 * Fixed size, which is what lets a backend drop its length bookkeeping
 * entirely -- see pf_ring.h. The name is a borrowed pointer rather than
 * inline bytes: names are string literals, so copying them would spend up
 * to 255 bytes and a memcpy per event to duplicate something immortal.
 * The drain thread dereferences the pointer and interns it, which keeps
 * both the strlen and the hashing off the hot path.
 *
 * @name and @value share eight bytes because no event carries both. A
 * slice or instant has a name and no value; a counter has a value and no
 * name, which is not a shortcut but Perfetto's own rule -- "most other
 * TrackEvent fields (e.g. categories, name, ..) are not supported for
 * TYPE_COUNTER events". RF_HAS_VALUE selects the arm.
 *
 *   0       8       16      24   28   32
 *   +-------+-------+-------+----+----+
 *   | ts    | track | name  |type|flag|
 *   |       |       | value |    |    |
 *   +-------+-------+-------+----+----+
 *
 * Overlaid on the ring rather than copied field by field, which is sound
 * because every member is naturally aligned: a backend hands out 8-byte
 * aligned payloads (PF_REC_ALIGN) and the two 4-byte fields sit in the
 * 24..31 tail. Any buffer this is cast onto must be 8-byte aligned too, as the ring's
 * slots and each backend's pop_buf are.
 */
struct pf_rec {
    uint64_t ts; /* timestamp, ns */
    uint64_t track; /* track_uuid */

    /* Borrowed string literal, or a counter's value; see RF_HAS_VALUE. */
    union {
        const char *name;
        uint64_t value;
    } u;

    uint32_t type; /* TYPE_* */
    uint32_t flags; /* see RF_* */
};

enum pf_rec_flag {
    RF_HAS_VALUE = 0x1u,
    RF_VALUE_IS_DOUBLE = 0x2u
};

/* The layout is the on-ring wire format, so pin it. The size is pinned
 * against the payload every backend carries, which is what the ring's
 * arithmetic assumes; the two must not drift apart. */
_Static_assert(sizeof(struct pf_rec) == PF_PAYLOAD_BYTES,
               "the record is exactly the ring's payload");
_Static_assert(offsetof(struct pf_rec, ts) == 0, "ts leads the record");
_Static_assert(offsetof(struct pf_rec, track) == 8, "track follows ts");
_Static_assert(offsetof(struct pf_rec, u) == 16, "name/value follows track");
_Static_assert(offsetof(struct pf_rec, type) == 24, "type follows name/value");
_Static_assert(offsetof(struct pf_rec, flags) == 28, "no padding before flags");

/* Cap on a track's stored name. Fixed so the track table is one
 * allocation at open and never grows. */
#define PF_TRACK_NAME_MAX 64

/* How much to presize the staging buffers for. These are hints, not
 * limits: an event name is a borrowed literal of any length, so pb_buf
 * grows if one exceeds this. Sized generously enough that the steady state
 * never reallocates -- pb_buf_warm's whole purpose -- while staying an
 * honest typical name rather than a worst case the ring no longer
 * imposes. */
#define PF_NAME_WARM 256u
#define PF_PACKET_WARM (PF_PAYLOAD_BYTES + PF_NAME_WARM)

/* Flush the staging buffer once it passes this, so one write() carries
 * many packets instead of one syscall per event. */
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
 * Track descriptors cannot live in the ring: it overwrites its oldest
 * records, so a descriptor emitted at startup would be long gone by the
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
};

/* --- Trace context ---------------------------------------------------- */

struct pf_trace {
    int fd;
    struct pf_config cfg;
    struct pf_ring ring;

    /* Continuous mode: ring occupancy, in slots, at which a producer wakes
     * the drain thread, from cfg.watermark_pct. */
    size_t watermark_slots;

    /* Serializes drains: a caller's pf_trace_dump() against the drain
     * thread, and against another caller's. Also covers the intern
     * tables, the staging buffers and tracks_emitted. */
    pthread_mutex_t out_lock;

    /* Guards the track table against concurrent pf_track_* calls, and
     * against a drain reading it. Not a hot-path lock. */
    pthread_mutex_t track_lock;
    struct track_def *tracks;
    unsigned track_count;
    unsigned tracks_emitted;
    uint64_t next_uuid;

    /* Drain thread wakeup. wake_pending is checked by producers without
     * the lock, so one futex is spent per drain cycle rather than per
     * event past the watermark. */
    pthread_t drain_thread;
    bool drain_thread_started;
    pthread_mutex_t drain_lock;
    pthread_cond_t drain_cv;
    _Atomic unsigned wake_pending;
    bool stop;

    /* Reused across dumps so steady-state draining allocates nothing. */
    struct pb_buf out;
    struct pb_buf scratch_a;
    struct pb_buf scratch_b;
    struct pb_buf scratch_c;
    /* Holds one InternedData entry while it is built, before being
     * embedded into scratch_a. Separate so the two never alias. */
    struct pb_buf scratch_entry;
    struct intern_table name_table;
    bool header_written;

    /* Raw format: one staging buffer per chunk kind. Each is emitted as a
     * chunk, in the order names/tracks then events, before the events
     * that reference them. */
    struct pb_buf raw_names;
    struct pb_buf raw_tracks;
    struct pb_buf raw_events;

    _Atomic uint64_t bytes_written;
};

/* --- Shared helpers, defined in pftrace.c ------------------------------ */

uint64_t now_ns(void);
int out_flush(struct pf_trace *t);
bool intern_lookup_or_insert(struct intern_table *tab, const char *key,
                             uint64_t *iid_out, bool *is_new_out);
void intern_table_reset(struct intern_table *tab);

/* --- The two output formats ------------------------------------------- */

/* Each is called from drain_locked with out_lock held. */
int pb_drain_locked(struct pf_trace *t); /* pftrace_pb.c */
int raw_drain_locked(struct pf_trace *t); /* pftrace_raw.c */
int raw_write_header(struct pf_trace *t); /* pftrace_raw.c */

#endif /* PFTRACE_INTERNAL_H */
