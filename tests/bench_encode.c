/* Prototype for TODO item 3: what protobuf encoding costs on the producer.
 *
 * The drain currently encodes each record into a TracePacket. Moving that onto
 * the emitting thread adds the same work to the caller. This measures that
 * work alone, on one thread, with no library and no drain: each event is
 * encoded into a packet buffer that is reset every chunk_slots records, the way
 * a chunk would fill, and compared against storing the same record as a plain
 * 48-byte struct, which is what the producer does today.
 *
 * The packet shape follows emit_record() in src/pftrace_pb.c: timestamp,
 * trusted sequence id, sequence flags, an interned name id on the first use of
 * a name, then the track event with its type, track uuid and name iid. Field
 * numbers are placeholders; they are one byte each, as in the real layout, so
 * the byte counts are representative.
 */

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pb_writer.h"

#define CHUNK_SLOTS 4096u
#define EVENTS 20000000u
#define NAMES 16u

/* Placeholder field numbers, all one-byte tags. */
#define F_TIMESTAMP 8
#define F_TRUSTED_SEQ 14
#define F_SEQ_FLAGS 13
#define F_INTERNED 12
#define F_TRACK_EVENT 11
#define F_EVENT_TYPE 9
#define F_EVENT_TRACK 11
#define F_EVENT_NAME_IID 10
#define F_IID 1

/* The same 48 bytes the chunk stores per event. */
struct raw_rec {
    uint64_t ts;
    uint64_t track;
    uint64_t name;
    uint32_t type;
    uint32_t flags;
    uint64_t arg[2];
};

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static const char *const kNames[NAMES] = {
    "parse", "decode", "lookup", "encode", "validate", "fetch", "merge", "sort",
    "hash",  "copy",   "filter", "reduce", "index",    "flush", "wait",  "notify",
};

/* Encodes one event into @ev, as a packet appended to @chunk. The interned
 * name is sent only on first use, which is how the cache is modelled here: a
 * per-writer table of NAMES entries. */
static void encode_event(struct pb_buf *chunk, struct pb_buf *ev, struct pb_buf *packet,
                         struct pb_buf *interned, struct pb_buf *entry, uint64_t ts, uint64_t track,
                         unsigned name_idx, bool first_use)
{
    pb_buf_reset(interned);
    pb_buf_reset(entry);
    pb_buf_reset(ev);
    pb_buf_reset(packet);

    if (first_use) {
        pb_put_varint_field(entry, F_IID, name_idx + 1);
        pb_put_string_field(entry, 2, kNames[name_idx], strlen(kNames[name_idx]));
        pb_put_submessage_field(interned, F_INTERNED, entry);
    }

    pb_put_varint_field(ev, F_EVENT_TYPE, 1);
    pb_put_varint_field(ev, F_EVENT_TRACK, track);
    pb_put_varint_field(ev, F_EVENT_NAME_IID, name_idx + 1);

    pb_put_varint_field(packet, F_TIMESTAMP, ts);
    pb_put_varint_field(packet, F_TRUSTED_SEQ, 1);
    pb_put_varint_field(packet, F_SEQ_FLAGS, 2);
    if (first_use)
        pb_put_submessage_field(packet, F_INTERNED, interned);
    pb_put_submessage_field(packet, F_TRACK_EVENT, ev);

    /* Stage the packet in the chunk, length-prefixed as the trace stream does. */
    pb_put_tag(chunk, 1, 2);
    pb_put_varint(chunk, packet->len);
    pb_put_bytes(chunk, packet->data, packet->len);
}

/* Direct encoder: writes the packet straight into the chunk, with no scratch
 * buffers or copies. A length is reserved as a 5-byte varint and filled in
 * when the body is done. A padded varint is valid protobuf, so the reader
 * takes it as an ordinary length. */
static uint8_t *w_varint(uint8_t *p, uint64_t v)
{
    while (v >= 0x80) {
        *p++ = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    *p++ = (uint8_t)v;
    return p;
}

static uint8_t *w_varint_field(uint8_t *p, uint32_t field_no, uint64_t v)
{
    *p++ = (uint8_t)(field_no << 3);
    return w_varint(p, v);
}

/* Writes a length-delimited field tag and reserves the 5 length bytes. Returns
 * the position of the reserved length. */
static uint8_t *w_begin_len(uint8_t *p, uint32_t field_no, uint8_t **slot)
{
    *p++ = (uint8_t)((field_no << 3) | 2);
    *slot = p;
    return p + 5;
}

static void w_end_len(uint8_t *slot, uint8_t *end)
{
    uint64_t len = (uint64_t)(end - (slot + 5));
    for (int i = 0; i < 4; i++) {
        slot[i] = (uint8_t)((len & 0x7f) | 0x80);
        len >>= 7;
    }
    slot[4] = (uint8_t)(len & 0x7f);
}

static uint8_t *encode_direct(uint8_t *p, uint64_t ts, uint64_t track, unsigned name_idx,
                              bool first_use)
{
    uint8_t *packet_len;
    uint8_t *interned_len = NULL;
    uint8_t *track_len;

    p = w_begin_len(p, 1, &packet_len); /* TracePacket in Trace.packet */
    p = w_varint_field(p, F_TIMESTAMP, ts);
    p = w_varint_field(p, F_TRUSTED_SEQ, 1);
    p = w_varint_field(p, F_SEQ_FLAGS, 2);
    if (first_use) {
        p = w_begin_len(p, F_INTERNED, &interned_len);
        p = w_varint_field(p, F_IID, name_idx + 1);
        const char *name = kNames[name_idx];
        size_t len = strlen(name);
        *p++ = 2 << 3 | 2;
        p = w_varint(p, len);
        for (size_t i = 0; i < len; i++)
            *p++ = (uint8_t)name[i];
        w_end_len(interned_len, p);
    }
    p = w_begin_len(p, F_TRACK_EVENT, &track_len);
    p = w_varint_field(p, F_EVENT_TYPE, 1);
    p = w_varint_field(p, F_EVENT_TRACK, track);
    p = w_varint_field(p, F_EVENT_NAME_IID, name_idx + 1);
    w_end_len(track_len, p);
    w_end_len(packet_len, p);
    return p;
}

int main(void)
{
    struct pb_buf chunk, ev, packet, interned, entry;
    pb_buf_init(&chunk);
    pb_buf_init(&ev);
    pb_buf_init(&packet);
    pb_buf_init(&interned);
    pb_buf_init(&entry);

    /* Warm the buffers so growth is not charged to the loop. */
    for (unsigned i = 0; i < 4; i++) {
        encode_event(&chunk, &ev, &packet, &interned, &entry, i, 1, i % NAMES, true);
    }
    pb_buf_reset(&chunk);

    /* Encoded: the producer builds a packet per event into a chunk. */
    uint64_t bytes = 0;
    uint64_t t0 = now_ns();
    unsigned in_chunk = 0;
    for (uint32_t i = 0; i < EVENTS; i++) {
        bool first = i < NAMES;
        encode_event(&chunk, &ev, &packet, &interned, &entry, i * 16u, 1, i % NAMES, first);
        if (++in_chunk == CHUNK_SLOTS) {
            bytes += chunk.len;
            pb_buf_reset(&chunk);
            in_chunk = 0;
        }
    }
    bytes += chunk.len;
    uint64_t t1 = now_ns();
    double encoded_ns = (double)(t1 - t0) / EVENTS;

    /* Direct: the packet is written into the chunk with no copies. */
    static uint8_t direct_chunk[CHUNK_SLOTS * 96];
    uint8_t *dp = direct_chunk;
    uint64_t direct_bytes = 0;
    t0 = now_ns();
    in_chunk = 0;
    for (uint32_t i = 0; i < EVENTS; i++) {
        bool first = i < NAMES;
        dp = encode_direct(dp, i * 16u, 1, i % NAMES, first);
        if (++in_chunk == CHUNK_SLOTS) {
            direct_bytes += (uint64_t)(dp - direct_chunk);
            dp = direct_chunk;
            in_chunk = 0;
        }
    }
    direct_bytes += (uint64_t)(dp - direct_chunk);
    t1 = now_ns();
    double direct_ns = (double)(t1 - t0) / EVENTS;

    /* Raw: the producer stores the 48-byte record as it does today. */
    struct raw_rec *recs = calloc(CHUNK_SLOTS, sizeof(*recs));
    if (!recs) {
        perror("calloc");
        return 1;
    }
    volatile uint64_t sink = 0;
    t0 = now_ns();
    in_chunk = 0;
    for (uint32_t i = 0; i < EVENTS; i++) {
        struct raw_rec *r = &recs[in_chunk];
        r->ts = i * 16u;
        r->track = 1;
        r->name = i % NAMES;
        r->type = 1;
        r->flags = 0;
        r->arg[0] = 0;
        r->arg[1] = 0;
        if (++in_chunk == CHUNK_SLOTS)
            in_chunk = 0;
    }
    sink += recs[0].ts;
    t1 = now_ns();
    double raw_ns = (double)(t1 - t0) / EVENTS;

    printf("raw 48-byte store:          %6.2f ns/event, 48.0 bytes/event\n", raw_ns);
    printf("encoded packet, per event:  %6.2f ns/event, %.1f bytes/event\n", encoded_ns,
           (double)bytes / EVENTS);
    printf("direct into chunk:          %6.2f ns/event, %.1f bytes/event\n", direct_ns,
           (double)direct_bytes / EVENTS);
    printf("encoding adds (direct):     %6.2f ns/event\n", direct_ns - raw_ns);

    free(recs);
    pb_buf_free(&chunk);
    pb_buf_free(&ev);
    pb_buf_free(&packet);
    pb_buf_free(&interned);
    pb_buf_free(&entry);
    return 0;
}
