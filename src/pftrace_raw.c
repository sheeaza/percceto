#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#include "pb_writer.h"
#include "pftrace_internal.h"

/* --- Raw format -------------------------------------------------------
 * The layout is specified in docs/raw_format.md. A file is a fixed header
 * followed by chunks, each an 8-byte header (kind, len) and a payload.
 * Every chunk goes out in one write(), so a crash leaves whole chunks and
 * at most one torn tail. Names and tracks are written as chunks ahead of
 * the events that reference them. */

#define RAW_MAGIC "PFRAWv1"
#define RAW_VERSION 1u
#define RAW_CHUNK_NAMES 1u
#define RAW_CHUNK_TRACKS 2u
#define RAW_CHUNK_EVENTS 3u

/* Appends the low @n bytes of @v, least significant first. */
static void raw_put_le(struct pb_buf *b, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        pb_put_byte(b, (uint8_t)(v >> (8 * i)));
}

/* Writes the file header. It is the first thing in a new raw file, and it
 * refuses a file that already has contents, so a raw file is never mixed
 * with another format. Returns 0, or -1 with errno set. */
int raw_write_header(struct pf_trace *t)
{
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
    errno = EINVAL;
    return -1;
#else
    struct stat st;
    if (fstat(t->fd, &st) != 0)
        return -1;
    if (st.st_size != 0) {
        errno = EEXIST;
        return -1;
    }

    pb_buf_reset(&t->out);
    pb_put_bytes(&t->out, RAW_MAGIC, sizeof(RAW_MAGIC)); /* 8, with NUL */
    raw_put_le(&t->out, RAW_VERSION, 4);
    raw_put_le(&t->out, sizeof(struct pf_rec), 4);
    raw_put_le(&t->out, (uint32_t)CLOCK_BOOTTIME, 4);
    raw_put_le(&t->out, 0, 4); /* flags, reserved */
    raw_put_le(&t->out, 0, 8); /* reserved */
    return out_flush(t);
#endif
}

/* Writes one chunk as a single write(), then empties the payload buffer.
 * An empty payload writes nothing. Caller holds out_lock. */
static int raw_write_chunk(struct pf_trace *t, uint32_t kind,
                           struct pb_buf *payload)
{
    if (payload->len == 0)
        return 0;

    pb_buf_reset(&t->out);
    raw_put_le(&t->out, kind, 4);
    raw_put_le(&t->out, payload->len, 4);
    pb_put_bytes(&t->out, payload->data, payload->len);
    pb_buf_reset(payload);
    return out_flush(t);
}

/* One TRACKS entry, 104 bytes: see docs/raw_format.md. */
static void raw_put_track(struct pb_buf *b, const struct track_def *td)
{
    size_t nlen = td->has_name ? strnlen(td->name, PF_TRACK_NAME_MAX) : 0;

    raw_put_le(b, td->uuid, 8);
    raw_put_le(b, td->parent_uuid, 8);
    raw_put_le(b, td->kind, 4);
    raw_put_le(b, (uint32_t)td->pid, 4);
    raw_put_le(b, (uint32_t)td->tid, 4);
    raw_put_le(b, td->unit, 4);
    raw_put_le(b, td->has_name, 4);
    raw_put_le(b, nlen, 4);
    pb_put_bytes(b, td->name, nlen);
    for (size_t i = nlen; i < PF_TRACK_NAME_MAX; i++)
        pb_put_byte(b, 0);
}

/* One NAMES entry: the pointer key, then the string. */
static void raw_put_name(struct pb_buf *b, const char *name, size_t nlen)
{
    raw_put_le(b, (uint64_t)(uintptr_t)name, 8);
    raw_put_le(b, nlen, 4);
    pb_put_bytes(b, name, nlen);
}

/* Queues descriptors for tracks registered since the last flush. The count
 * is read at flush time, after every queued event was collected, so each
 * track an event refers to is already queued or already written. Caller
 * holds out_lock. */
static void raw_queue_tracks(struct pf_trace *t)
{
    pthread_mutex_lock(&t->track_lock);
    unsigned count = t->track_count;
    pthread_mutex_unlock(&t->track_lock);

    for (unsigned i = 0; i < count; i++) {
        struct track_def td;
        if (track_emit_due(t, i, t->tracks_emitted, &td))
            raw_put_track(&t->raw.tracks, &td);
    }
    t->tracks_emitted = count;
}

/* Writes the pending TRACKS, NAMES and EVENTS chunks, in that order, so
 * every definition precedes the events that use it. Caller holds out_lock. */
static int raw_flush(struct pf_trace *t)
{
    raw_queue_tracks(t);
    if (raw_write_chunk(t, RAW_CHUNK_TRACKS, &t->raw.tracks) != 0)
        return -1;
    if (raw_write_chunk(t, RAW_CHUNK_NAMES, &t->raw.names) != 0)
        return -1;
    return raw_write_chunk(t, RAW_CHUNK_EVENTS, &t->raw.events);
}

/* Queues one record for the raw file. The name is queued as a definition
 * the first time the drain sees its pointer. Caller holds out_lock. */
static int raw_emit_record(struct pf_trace *t, const uint8_t *rec)
{
    const struct pf_rec *r = (const struct pf_rec *)rec;
    bool has_value = (r->flags & RF_HAS_VALUE) != 0;
    const char *name = has_value ? NULL : r->u.name;

    if (name) {
        uint64_t iid;
        bool is_new = false;
        bool found =
            intern_lookup_or_insert(&t->name_table, name, &iid, &is_new);
        /* A full table cannot dedupe, so the definition is queued again.
         * Harmless: the reader keeps the first, and the copies are identical. */
        if (!found || is_new)
            raw_put_name(&t->raw.names, name, strlen(name));
    }

    /* The record is copied as is; see struct pf_rec for the layout. */
    pb_put_bytes(&t->raw.events, rec, PF_PAYLOAD_BYTES);
    if (t->raw.events.len >= PF_OUT_FLUSH_THRESHOLD)
        return raw_flush(t);
    return 0;
}

/* Caller holds out_lock. */
int raw_drain_locked(struct pf_trace *t)
{
    struct pf_ring_reader rd;
    pf_ring_read_begin(&t->ring, &rd);

    const uint8_t *rec;
    while ((rec = pf_ring_next(&t->ring, &rd)) != NULL) {
        if (raw_emit_record(t, rec) != 0)
            return -1;
    }

    pf_ring_read_commit(&t->ring, &rd);
    return raw_flush(t);
}
