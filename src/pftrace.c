#define _GNU_SOURCE

#include "pftrace.h"
#include "pb_writer.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- Confirmed Perfetto wire-format field numbers -------------------
 * Pulled directly from the compiled perfetto_trace_pb2 DESCRIPTOR (the
 * official schema), not from memory. See plan doc for the full table. */

/* Trace */
#define FN_TRACE_PACKET 1

/* TracePacket */
#define FN_PACKET_TIMESTAMP 8
#define FN_PACKET_TRUSTED_SEQ_ID 10
#define FN_PACKET_TRACK_EVENT 11
#define FN_PACKET_INTERNED_DATA 12
#define FN_PACKET_SEQUENCE_FLAGS 13
#define FN_PACKET_TRACK_DESCRIPTOR 60

#define SEQ_INCREMENTAL_STATE_CLEARED 1u
#define SEQ_NEEDS_INCREMENTAL_STATE 2u

/* TrackEvent */
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

/* TrackDescriptor */
#define FN_TRACK_UUID 1
#define FN_TRACK_NAME 2
#define FN_TRACK_PROCESS 3
#define FN_TRACK_THREAD 4
#define FN_TRACK_PARENT_UUID 5
#define FN_TRACK_COUNTER 8

/* ThreadDescriptor / ProcessDescriptor */
#define FN_THREAD_PID 1
#define FN_THREAD_TID 2
#define FN_THREAD_NAME 5
#define FN_PROCESS_PID 1
#define FN_PROCESS_NAME 6

/* CounterDescriptor */
#define FN_COUNTER_UNIT 3

/* InternedData / EventName / EventCategory */
#define FN_INTERNED_EVENT_CATEGORIES 1
#define FN_INTERNED_EVENT_NAMES 2
#define FN_IID 1
#define FN_NAME_STR 2

/* --- Interning tables -------------------------------------------------
 * Fixed-capacity open-addressing hash tables (string -> iid). If a table
 * ever fills up, callers fall back to inline string fields instead of
 * iids — always correct, just less deduplicated. */

#define PF_INTERN_CAP 1021 /* prime */

typedef struct {
    char *key; /* NULL = empty slot */
    uint64_t iid;
} intern_slot;

typedef struct {
    intern_slot slots[PF_INTERN_CAP];
    size_t count;
    uint64_t next_iid;
} intern_table;

static uint64_t fnv1a(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

/* Returns true if `key` is now represented by *iid_out (found or newly
 * inserted); *is_new_out tells the caller whether this is a fresh
 * entry that still needs an InternedData record emitted. Returns false
 * only when the table is full and `key` wasn't already present. */
static bool intern_lookup_or_insert(intern_table *tab, const char *key, uint64_t *iid_out, bool *is_new_out) {
    size_t idx = (size_t)(fnv1a(key) % PF_INTERN_CAP);
    for (size_t probe = 0; probe < PF_INTERN_CAP; probe++) {
        intern_slot *slot = &tab->slots[idx];
        if (slot->key == NULL) {
            if (tab->count >= PF_INTERN_CAP) return false;
            slot->key = strdup(key);
            if (!slot->key) {
                fprintf(stderr, "pftrace: fatal: out of memory\n");
                abort();
            }
            slot->iid = tab->next_iid++;
            tab->count++;
            *iid_out = slot->iid;
            *is_new_out = true;
            return true;
        }
        if (strcmp(slot->key, key) == 0) {
            *iid_out = slot->iid;
            *is_new_out = false;
            return true;
        }
        idx = (idx + 1) % PF_INTERN_CAP;
    }
    return false;
}

static void intern_table_init(intern_table *tab) {
    memset(tab, 0, sizeof(*tab));
    tab->next_iid = 1;
}

static void intern_table_free(intern_table *tab) {
    for (size_t i = 0; i < PF_INTERN_CAP; i++) free(tab->slots[i].key);
}

/* --- Trace context ---------------------------------------------------- */

struct pf_trace {
    FILE *f;
    pthread_mutex_t lock;
    uint64_t next_uuid;
    uint32_t seq_id;
    bool first_packet_written;
    intern_table name_table;
    intern_table cat_table;
};

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint32_t next_seq_flags(pf_trace_t *t, uint32_t extra) {
    uint32_t flags = extra;
    if (!t->first_packet_written) {
        flags |= SEQ_INCREMENTAL_STATE_CLEARED;
        t->first_packet_written = true;
    }
    return flags;
}

/* Frames `packet` as one entry of the (implicit, streamed) Trace.packet
 * repeated field and writes it out. */
static void write_packet(pf_trace_t *t, const pb_buf *packet) {
    pb_buf hdr;
    pb_buf_init(&hdr);
    pb_put_tag(&hdr, FN_TRACE_PACKET, 2);
    pb_put_varint(&hdr, packet->len);
    fwrite(hdr.data, 1, hdr.len, t->f);
    fwrite(packet->data, 1, packet->len, t->f);
    pb_buf_free(&hdr);
}

static void add_interned_entry(pb_buf *interned, uint32_t field_no, uint64_t iid, const char *name) {
    pb_buf entry;
    pb_buf_init(&entry);
    pb_put_varint_field(&entry, FN_IID, iid);
    pb_put_string_field(&entry, FN_NAME_STR, name, strlen(name));
    pb_put_submessage_field(interned, field_no, &entry);
    pb_buf_free(&entry);
}

pf_trace_t *pf_trace_open(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return NULL;
    pf_trace_t *t = calloc(1, sizeof(*t));
    if (!t) {
        fclose(f);
        return NULL;
    }
    t->f = f;
    pthread_mutex_init(&t->lock, NULL);
    t->next_uuid = 1;
    t->seq_id = 1;
    t->first_packet_written = false;
    intern_table_init(&t->name_table);
    intern_table_init(&t->cat_table);
    return t;
}

void pf_trace_flush(pf_trace_t *t) {
    pthread_mutex_lock(&t->lock);
    fflush(t->f);
    pthread_mutex_unlock(&t->lock);
}

void pf_trace_close(pf_trace_t *t) {
    if (!t) return;
    fflush(t->f);
    fclose(t->f);
    pthread_mutex_destroy(&t->lock);
    intern_table_free(&t->name_table);
    intern_table_free(&t->cat_table);
    free(t);
}

/* --- Tracks ------------------------------------------------------------ */

uint64_t pf_track_process(pf_trace_t *t, int32_t pid, const char *name) {
    pthread_mutex_lock(&t->lock);
    uint64_t uuid = t->next_uuid++;

    pb_buf proc;
    pb_buf_init(&proc);
    pb_put_varint_field(&proc, FN_PROCESS_PID, (uint64_t)(uint32_t)pid);
    if (name) pb_put_string_field(&proc, FN_PROCESS_NAME, name, strlen(name));

    pb_buf desc;
    pb_buf_init(&desc);
    pb_put_varint_field(&desc, FN_TRACK_UUID, uuid);
    pb_put_submessage_field(&desc, FN_TRACK_PROCESS, &proc);
    pb_buf_free(&proc);

    pb_buf packet;
    pb_buf_init(&packet);
    pb_put_varint_field(&packet, FN_PACKET_TIMESTAMP, now_ns());
    pb_put_varint_field(&packet, FN_PACKET_TRUSTED_SEQ_ID, t->seq_id);
    uint32_t flags = next_seq_flags(t, 0);
    if (flags) pb_put_varint_field(&packet, FN_PACKET_SEQUENCE_FLAGS, flags);
    pb_put_submessage_field(&packet, FN_PACKET_TRACK_DESCRIPTOR, &desc);
    pb_buf_free(&desc);

    write_packet(t, &packet);
    pb_buf_free(&packet);
    pthread_mutex_unlock(&t->lock);
    return uuid;
}

uint64_t pf_track_thread(pf_trace_t *t, uint64_t parent_uuid, int32_t pid, int32_t tid, const char *name) {
    pthread_mutex_lock(&t->lock);
    uint64_t uuid = t->next_uuid++;

    pb_buf thread;
    pb_buf_init(&thread);
    pb_put_varint_field(&thread, FN_THREAD_PID, (uint64_t)(uint32_t)pid);
    pb_put_varint_field(&thread, FN_THREAD_TID, (uint64_t)(uint32_t)tid);
    if (name) pb_put_string_field(&thread, FN_THREAD_NAME, name, strlen(name));

    pb_buf desc;
    pb_buf_init(&desc);
    pb_put_varint_field(&desc, FN_TRACK_UUID, uuid);
    if (parent_uuid) pb_put_varint_field(&desc, FN_TRACK_PARENT_UUID, parent_uuid);
    pb_put_submessage_field(&desc, FN_TRACK_THREAD, &thread);
    pb_buf_free(&thread);

    pb_buf packet;
    pb_buf_init(&packet);
    pb_put_varint_field(&packet, FN_PACKET_TIMESTAMP, now_ns());
    pb_put_varint_field(&packet, FN_PACKET_TRUSTED_SEQ_ID, t->seq_id);
    uint32_t flags = next_seq_flags(t, 0);
    if (flags) pb_put_varint_field(&packet, FN_PACKET_SEQUENCE_FLAGS, flags);
    pb_put_submessage_field(&packet, FN_PACKET_TRACK_DESCRIPTOR, &desc);
    pb_buf_free(&desc);

    write_packet(t, &packet);
    pb_buf_free(&packet);
    pthread_mutex_unlock(&t->lock);
    return uuid;
}

uint64_t pf_track_counter(pf_trace_t *t, uint64_t parent_uuid, const char *name, pf_unit_t unit) {
    pthread_mutex_lock(&t->lock);
    uint64_t uuid = t->next_uuid++;

    pb_buf counter;
    pb_buf_init(&counter);
    if (unit != PF_UNIT_UNSPECIFIED) pb_put_varint_field(&counter, FN_COUNTER_UNIT, (uint64_t)unit);

    pb_buf desc;
    pb_buf_init(&desc);
    pb_put_varint_field(&desc, FN_TRACK_UUID, uuid);
    if (parent_uuid) pb_put_varint_field(&desc, FN_TRACK_PARENT_UUID, parent_uuid);
    if (name) pb_put_string_field(&desc, FN_TRACK_NAME, name, strlen(name));
    pb_put_submessage_field(&desc, FN_TRACK_COUNTER, &counter);
    pb_buf_free(&counter);

    pb_buf packet;
    pb_buf_init(&packet);
    pb_put_varint_field(&packet, FN_PACKET_TIMESTAMP, now_ns());
    pb_put_varint_field(&packet, FN_PACKET_TRUSTED_SEQ_ID, t->seq_id);
    uint32_t flags = next_seq_flags(t, 0);
    if (flags) pb_put_varint_field(&packet, FN_PACKET_SEQUENCE_FLAGS, flags);
    pb_put_submessage_field(&packet, FN_PACKET_TRACK_DESCRIPTOR, &desc);
    pb_buf_free(&desc);

    write_packet(t, &packet);
    pb_buf_free(&packet);
    pthread_mutex_unlock(&t->lock);
    return uuid;
}

/* --- Slices / instants --------------------------------------------------
 * Shared by pf_slice_begin (TYPE_SLICE_BEGIN) and pf_instant_event
 * (TYPE_INSTANT) — both are a named, optionally-categorized TrackEvent
 * that may need to intern a fresh name/category. They differ only in
 * the `type` value and in whether a matching _end call follows. */

static void emit_named_event(pf_trace_t *t, uint32_t type, uint64_t track_uuid, const char *name, const char *category) {
    pthread_mutex_lock(&t->lock);

    pb_buf interned;
    pb_buf_init(&interned);
    bool have_interned = false;

    uint64_t name_iid = 0;
    bool name_interned = false;
    if (name) {
        bool is_new = false;
        name_interned = intern_lookup_or_insert(&t->name_table, name, &name_iid, &is_new);
        if (name_interned && is_new) {
            add_interned_entry(&interned, FN_INTERNED_EVENT_NAMES, name_iid, name);
            have_interned = true;
        }
    }

    uint64_t cat_iid = 0;
    bool cat_interned = false;
    if (category) {
        bool is_new = false;
        cat_interned = intern_lookup_or_insert(&t->cat_table, category, &cat_iid, &is_new);
        if (cat_interned && is_new) {
            add_interned_entry(&interned, FN_INTERNED_EVENT_CATEGORIES, cat_iid, category);
            have_interned = true;
        }
    }

    pb_buf ev;
    pb_buf_init(&ev);
    pb_put_varint_field(&ev, FN_EVENT_TYPE, type);
    pb_put_varint_field(&ev, FN_EVENT_TRACK_UUID, track_uuid);
    if (name) {
        if (name_interned) pb_put_varint_field(&ev, FN_EVENT_NAME_IID, name_iid);
        else pb_put_string_field(&ev, FN_EVENT_NAME_STR, name, strlen(name));
    }
    if (category) {
        if (cat_interned) pb_put_varint_field(&ev, FN_EVENT_CATEGORY_IIDS, cat_iid);
        else pb_put_string_field(&ev, FN_EVENT_CATEGORIES_STR, category, strlen(category));
    }

    pb_buf packet;
    pb_buf_init(&packet);
    pb_put_varint_field(&packet, FN_PACKET_TIMESTAMP, now_ns());
    pb_put_varint_field(&packet, FN_PACKET_TRUSTED_SEQ_ID, t->seq_id);
    pb_put_varint_field(&packet, FN_PACKET_SEQUENCE_FLAGS, next_seq_flags(t, SEQ_NEEDS_INCREMENTAL_STATE));
    if (have_interned) pb_put_submessage_field(&packet, FN_PACKET_INTERNED_DATA, &interned);
    pb_put_submessage_field(&packet, FN_PACKET_TRACK_EVENT, &ev);

    write_packet(t, &packet);

    pb_buf_free(&packet);
    pb_buf_free(&ev);
    pb_buf_free(&interned);
    pthread_mutex_unlock(&t->lock);
}

void pf_slice_begin(pf_trace_t *t, uint64_t track_uuid, const char *name, const char *category) {
    emit_named_event(t, TYPE_SLICE_BEGIN, track_uuid, name, category);
}

void pf_instant_event(pf_trace_t *t, uint64_t track_uuid, const char *name, const char *category) {
    emit_named_event(t, TYPE_INSTANT, track_uuid, name, category);
}

void pf_slice_end(pf_trace_t *t, uint64_t track_uuid) {
    pthread_mutex_lock(&t->lock);

    pb_buf ev;
    pb_buf_init(&ev);
    pb_put_varint_field(&ev, FN_EVENT_TYPE, TYPE_SLICE_END);
    pb_put_varint_field(&ev, FN_EVENT_TRACK_UUID, track_uuid);

    pb_buf packet;
    pb_buf_init(&packet);
    pb_put_varint_field(&packet, FN_PACKET_TIMESTAMP, now_ns());
    pb_put_varint_field(&packet, FN_PACKET_TRUSTED_SEQ_ID, t->seq_id);
    pb_put_varint_field(&packet, FN_PACKET_SEQUENCE_FLAGS, next_seq_flags(t, SEQ_NEEDS_INCREMENTAL_STATE));
    pb_put_submessage_field(&packet, FN_PACKET_TRACK_EVENT, &ev);

    write_packet(t, &packet);

    pb_buf_free(&packet);
    pb_buf_free(&ev);
    pthread_mutex_unlock(&t->lock);
}

/* --- Counters ------------------------------------------------------------ */

static void emit_counter_packet(pf_trace_t *t, uint64_t counter_track_uuid, const pb_buf *value_field_encoded) {
    pb_buf ev;
    pb_buf_init(&ev);
    pb_put_varint_field(&ev, FN_EVENT_TYPE, TYPE_COUNTER);
    pb_put_varint_field(&ev, FN_EVENT_TRACK_UUID, counter_track_uuid);
    pb_put_bytes(&ev, value_field_encoded->data, value_field_encoded->len);

    pb_buf packet;
    pb_buf_init(&packet);
    pb_put_varint_field(&packet, FN_PACKET_TIMESTAMP, now_ns());
    pb_put_varint_field(&packet, FN_PACKET_TRUSTED_SEQ_ID, t->seq_id);
    pb_put_varint_field(&packet, FN_PACKET_SEQUENCE_FLAGS, next_seq_flags(t, SEQ_NEEDS_INCREMENTAL_STATE));
    pb_put_submessage_field(&packet, FN_PACKET_TRACK_EVENT, &ev);

    write_packet(t, &packet);

    pb_buf_free(&packet);
    pb_buf_free(&ev);
}

void pf_counter_set_int(pf_trace_t *t, uint64_t counter_track_uuid, int64_t value) {
    pthread_mutex_lock(&t->lock);
    pb_buf val;
    pb_buf_init(&val);
    pb_put_varint_field(&val, FN_EVENT_COUNTER_VALUE, (uint64_t)value);
    emit_counter_packet(t, counter_track_uuid, &val);
    pb_buf_free(&val);
    pthread_mutex_unlock(&t->lock);
}

void pf_counter_set_double(pf_trace_t *t, uint64_t counter_track_uuid, double value) {
    pthread_mutex_lock(&t->lock);
    pb_buf val;
    pb_buf_init(&val);
    pb_put_double_field(&val, FN_EVENT_DOUBLE_COUNTER_VALUE, value);
    emit_counter_packet(t, counter_track_uuid, &val);
    pb_buf_free(&val);
    pthread_mutex_unlock(&t->lock);
}
