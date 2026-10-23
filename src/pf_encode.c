/* Producer-side protobuf encoding. See pf_encode.h.
 *
 * Each length prefix is computed from the fields before the packet is written,
 * so every byte is written once. The *_len helpers and the writers below must
 * agree on each field's size; pf_pb_size and pf_pb_encode both use them. */

#include <string.h>

#include "pf_encode.h"
#include "pftrace_internal.h"

struct enc {
    uint8_t *base;
    size_t pos;
};

/* One event, as the encoder takes it. */
struct pb_event {
    clockid_t clk; /* trace clock, named on the packet's timestamp */
    uint64_t ts;
    uint32_t seq_id; /* trusted_packet_sequence_id: the writer's sequence */
    uint32_t type; /* TYPE_* */
    uint64_t track; /* track_uuid */

    /* The name. A name the writer has interned is sent as iid, and its
     * definition rides in this packet when @define is set. Otherwise the
     * characters go inline, which is how formatted names and names the cache
     * cannot hold are sent. */
    bool use_iid;
    bool define;
    uint64_t iid;
    const char *name;
    size_t nlen;

    bool has_value; /* counters: the value replaces the name */
    bool value_is_double;
    uint64_t value;

    /* Body lengths, set by pf_pb_size: the TrackEvent and the Trace.packet. */
    size_t event_len;
    size_t body_len;
};

static void e_byte(struct enc *e, uint8_t b)
{
    e->base[e->pos++] = b;
}

static void e_varint(struct enc *e, uint64_t v)
{
    while (v >= 0x80) {
        e_byte(e, (uint8_t)(v | 0x80));
        v >>= 7;
    }
    e_byte(e, (uint8_t)v);
}

static void e_tag(struct enc *e, uint32_t field, uint32_t wire)
{
    e_varint(e, ((uint64_t)field << 3) | wire);
}

static void e_vfield(struct enc *e, uint32_t field, uint64_t v)
{
    e_tag(e, field, 0);
    e_varint(e, v);
}

static void e_sfield(struct enc *e, uint32_t field, const char *s, size_t n)
{
    e_tag(e, field, 2);
    e_varint(e, n);
    memcpy(e->base + e->pos, s, n);
    e->pos += n;
}

/* Little-endian, as protobuf fixed64 is on the wire. */
static void e_fixed64(struct enc *e, uint32_t field, uint64_t v)
{
    e_tag(e, field, 1);
    for (unsigned i = 0; i < 8; i++)
        e_byte(e, (uint8_t)(v >> (8 * i)));
}

/* Encoded sizes, from the same field list the writers use. */

static size_t varint_len(uint64_t v)
{
    size_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        n++;
    }
    return n;
}

static size_t vfield_len(uint32_t field, uint64_t v)
{
    return varint_len((uint64_t)field << 3) + varint_len(v);
}

static size_t sfield_len(uint32_t field, size_t n)
{
    return varint_len(((uint64_t)field << 3) | 2) + varint_len(n) + n;
}

static size_t fixed64_len(uint32_t field)
{
    return varint_len(((uint64_t)field << 3) | 1) + 8;
}

/* A length-delimited field whose body is @body bytes long. */
static size_t msg_len(uint32_t field, size_t body)
{
    return varint_len(((uint64_t)field << 3) | 2) + varint_len(body) + body;
}

static size_t name_entry_len(const struct pb_event *v)
{
    return vfield_len(FN_IID, v->iid) + sfield_len(FN_NAME_STR, v->nlen);
}

static size_t event_body_len(const struct pb_event *v)
{
    size_t n = vfield_len(FN_EVENT_TYPE, v->type) + vfield_len(FN_EVENT_TRACK_UUID, v->track);
    if (v->use_iid)
        n += vfield_len(FN_EVENT_NAME_IID, v->iid);
    else if (v->nlen)
        n += sfield_len(FN_EVENT_NAME_STR, v->nlen);
    if (v->has_value)
        n += v->value_is_double ? fixed64_len(FN_EVENT_DOUBLE_COUNTER_VALUE) :
                                  vfield_len(FN_EVENT_COUNTER_VALUE, v->value);
    return n;
}

/* Writers. Each one's output matches its *_len above byte for byte. */

static void enc_name_entry(struct enc *e, const struct pb_event *v)
{
    e_vfield(e, FN_IID, v->iid);
    e_sfield(e, FN_NAME_STR, v->name, v->nlen);
}

static void enc_event_body(struct enc *e, const struct pb_event *v)
{
    e_vfield(e, FN_EVENT_TYPE, v->type);
    e_vfield(e, FN_EVENT_TRACK_UUID, v->track);
    if (v->use_iid)
        e_vfield(e, FN_EVENT_NAME_IID, v->iid);
    else if (v->nlen)
        e_sfield(e, FN_EVENT_NAME_STR, v->name, v->nlen);
    if (v->has_value) {
        if (v->value_is_double)
            e_fixed64(e, FN_EVENT_DOUBLE_COUNTER_VALUE, v->value);
        else
            e_vfield(e, FN_EVENT_COUNTER_VALUE, v->value);
    }
}

static void enc_packet_body(struct enc *e, const struct pb_event *v)
{
    e_vfield(e, FN_PACKET_TIMESTAMP, v->ts);
    e_vfield(e, FN_PACKET_TIMESTAMP_CLOCK_ID, builtin_clock(v->clk));
    e_vfield(e, FN_PACKET_TRUSTED_SEQ_ID, v->seq_id);
    e_vfield(e, FN_PACKET_SEQUENCE_FLAGS, SEQ_NEEDS_INCREMENTAL_STATE);
    if (v->define) {
        e_tag(e, FN_PACKET_INTERNED_DATA, 2);
        e_varint(e, msg_len(FN_INTERNED_EVENT_NAMES, name_entry_len(v)));
        e_tag(e, FN_INTERNED_EVENT_NAMES, 2);
        e_varint(e, name_entry_len(v));
        enc_name_entry(e, v);
    }
    e_tag(e, FN_PACKET_TRACK_EVENT, 2);
    e_varint(e, v->event_len);
    enc_event_body(e, v);
}

/* Sizes @v: sets its body lengths and returns the length of @v encoded as one
 * Trace.packet, length prefix included, without encoding it. The encoder reuses
 * the lengths, so each is computed once per event. */
static size_t pf_pb_size(struct pb_event *v)
{
    v->event_len = event_body_len(v);
    v->body_len = vfield_len(FN_PACKET_TIMESTAMP, v->ts) +
                  vfield_len(FN_PACKET_TIMESTAMP_CLOCK_ID, builtin_clock(v->clk)) +
                  vfield_len(FN_PACKET_TRUSTED_SEQ_ID, v->seq_id) +
                  vfield_len(FN_PACKET_SEQUENCE_FLAGS, SEQ_NEEDS_INCREMENTAL_STATE) +
                  (v->define ? msg_len(FN_PACKET_INTERNED_DATA,
                                       msg_len(FN_INTERNED_EVENT_NAMES, name_entry_len(v))) :
                               0) +
                  msg_len(FN_PACKET_TRACK_EVENT, v->event_len);
    return msg_len(FN_TRACE_PACKET, v->body_len);
}

/* Encodes @v, sized by pf_pb_size, as one Trace.packet into @dst, which must hold
 * the length pf_pb_size returned. Returns the length. */
static size_t pf_pb_encode(uint8_t *dst, const struct pb_event *v)
{
    struct enc e = { dst, 0 };
    e_tag(&e, FN_TRACE_PACKET, 2);
    e_varint(&e, v->body_len);
    enc_packet_body(&e, v);
    return e.pos;
}

/* Cache scan. A slot holding @name in the current epoch is used as it is. A
 * slot holding it in an older epoch is redefined here. A new name takes the
 * first slot whose definition is from an older epoch, since a slot defined in
 * this chunk may already be named by a record above it. */
/* Looks up @name in @w's cache. Returns its slot, or -1 when the cache has no
 * slot free for it. Sets *define when the name's definition must go into the
 * current chunk. Read-only, so nothing changes until the record is placed. */
static int pf_pb_lookup(const struct pf_writer *w, const char *name, bool *define)
{
    int stale = -1;
    for (int i = 0; i < PF_PB_INTERN_SLOTS; i++) {
        const struct pb_intern_slot *s = &w->intern[i];
        if (s->key == name) {
            *define = s->epoch != w->epoch;
            return i;
        }
        if (stale < 0 && s->epoch != w->epoch)
            stale = i;
    }
    *define = true;
    return stale;
}

/* Records that @slot now holds @name, of length @nlen, defined in the current
 * chunk. */
static void pf_pb_commit_name(struct pf_writer *w, int slot, const char *name, size_t nlen)
{
    w->intern[slot].key = name;
    w->intern[slot].len = nlen;
    w->intern[slot].epoch = w->epoch;
}

void pf_pb_emit(struct pf_trace *t, struct pf_writer *w, uint64_t ts, uint32_t type, uint64_t track,
                const char *name, bool dynamic, bool has_value, uint64_t raw_value,
                bool value_is_double)
{
    /* A dynamic name is built by the caller, so it differs per event. It is
     * sent inline and never interned: a pointer key would name the wrong
     * string once the caller reuses its buffer. */
    const char *shown = has_value ? NULL : name;
    size_t nlen = 0;
    char fit[PF_DYN_NAME_MAX + 1];
    if (dynamic && shown) {
        bool cut = false;
        nlen = pf_dyn_name_fit(shown, &cut);
        pf_dyn_name_copy(fit, shown, nlen, cut);
        fit[nlen] = '\0';
        shown = fit;
        if (cut)
            atomic_fetch_add_explicit(&t->truncated, 1, memory_order_relaxed);
    }

    struct pb_event v;
    size_t size;
    int slot;
    bool define;
    for (;;) {
        memset(&v, 0, sizeof v);
        v.clk = t->cfg.clock_id;
        v.ts = ts;
        v.seq_id = w->id + 1;
        v.type = type;
        v.track = track;
        v.has_value = has_value;
        v.value_is_double = value_is_double;
        v.value = raw_value;

        slot = -1;
        define = false;
        if (!dynamic && shown && shown[0])
            slot = pf_pb_lookup(w, shown, &define);
        /* A hit carries the name's length from when it was defined. */
        if (!dynamic && shown)
            nlen = (slot >= 0 && w->intern[slot].key == shown) ? w->intern[slot].len :
                                                                 strlen(shown);
        v.use_iid = slot >= 0;
        v.define = v.use_iid && define;
        v.iid = (uint64_t)slot + 1;
        v.name = shown;
        v.nlen = nlen;

        size = pf_pb_size(&v);
        if (size > t->chunk_payload) {
            /* Too big for any chunk; a record is never split. */
            atomic_fetch_add_explicit(&t->dropped, 1, memory_order_relaxed);
            return;
        }
        if (w->cur && w->used + size <= t->chunk_payload)
            break;
        /* Rotation changes the epoch, so the name is looked up again. */
        if (!pf_writer_rotate(t, w))
            return; /* no chunk free; pf_writer_rotate counted the drop */
    }

    if (v.define)
        pf_pb_commit_name(w, slot, shown, nlen);
    pf_pb_encode(w->cur->bytes + w->used, &v);
    w->used += (uint32_t)size;

    atomic_store_explicit(&w->cur->used, w->used, memory_order_release);
    atomic_store_explicit(&w->events, atomic_load_explicit(&w->events, memory_order_relaxed) + 1,
                          memory_order_relaxed);
}
