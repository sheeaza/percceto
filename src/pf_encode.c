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

/* Per-argument state, set by pf_pb_args_prep and pf_pb_resolve_keys. Kept out
 * of pb_event: an event with no arguments then zeroes no argument state, and
 * points at none. */
struct pb_args {
    size_t key_len[PF_MAX_ARGS];
    size_t ann_len[PF_MAX_ARGS];
    /* The key's iid, or 0 when the key goes inline. key_slot and key_def say
     * which cache slot it uses and whether this packet defines it. */
    uint64_t key_iid[PF_MAX_ARGS];
    int key_slot[PF_MAX_ARGS];
    bool key_def[PF_MAX_ARGS];
};

/* One event, as the encoder takes it. */
struct pb_event {
    clockid_t clk; /* trace clock, which the chunk's header ties to its own */
    /* The event's time, as a delta from the previous event in the chunk on the
     * incremental clock. The first event is relative to the header's reading,
     * which is its own time, so its delta is 0. An @absolute event carries its
     * time on @clk instead, and names it. */
    uint64_t ts;
    bool absolute;
    uint32_t seq_id; /* trusted_packet_sequence_id: the writer's sequence */
    uint32_t type; /* TYPE_* */
    /* track_uuid. Left out when @on_default, for an event on the chunk's
     * default track, which the header's defaults name. A flag, not a 0
     * track: 0 is what a failed registration returns, and an event on it
     * must still say so rather than land on the default. */
    uint64_t track;
    bool on_default;

    /* The chunk's header packet, written ahead of the event when the event
     * opens a chunk. It clears the sequence's state, sets the packet defaults,
     * and ties the incremental clock to @clk at @head_ts. */
    bool head;
    bool head_snap; /* the header ties the incremental clock; see head_clock */
    uint64_t head_ts;
    uint64_t head_track;
    size_t head_body_len;

    /* The name. A name the writer has interned is sent as iid, and its
     * definition rides in this packet when @define is set. Otherwise the
     * characters go inline, which is how formatted names and names the cache
     * cannot hold are sent. */
    bool use_iid;
    bool define;
    uint64_t iid;
    const char *name;
    size_t nlen;
    /* A dynamic name longer than PF_DYN_NAME_MAX: @nlen is the cap, and the
     * encoder writes "..." over its last three bytes. */
    bool cut;

    bool has_value; /* counters: the value replaces the name */
    bool value_is_double;
    uint64_t value;

    /* Typed arguments, from the caller. ax holds their state, set by
     * pf_pb_args_prep and pf_pb_resolve_keys, and is NULL when there are none. */
    unsigned nargs;
    const struct pf_arg *args;
    struct pb_args *ax;

    /* Lengths, set by pf_pb_size: the InternedData this packet carries, the
     * TrackEvent, and the Trace.packet. */
    size_t intern_len;
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

/* A string field whose last three bytes become "..." when @cut, so a long
 * dynamic name is cut as it is written, without a copy of its own. */
static void e_name_sfield(struct enc *e, uint32_t field, const char *s, size_t n, bool cut)
{
    e_tag(e, field, 2);
    e_varint(e, n);
    pf_dyn_name_copy((char *)e->base + e->pos, s, n, cut);
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

/* An InternedString body: an iid, then the string. Event names and argument
 * keys both use this shape. */
static size_t str_entry_len(uint64_t iid, size_t nlen)
{
    return vfield_len(FN_IID, iid) + sfield_len(FN_NAME_STR, nlen);
}

static size_t name_entry_len(const struct pb_event *v)
{
    return str_entry_len(v->iid, v->nlen);
}

/* The body of argument @i's DebugAnnotation: its name, then its value. Needs
 * the lengths and keys pf_pb_args_prep and pf_pb_resolve_keys set. */
static size_t ann_body_len(const struct pb_event *v, unsigned i)
{
    const struct pf_arg *a = &v->args[i];
    size_t n = v->ax->key_iid[i] ? vfield_len(FN_ANN_NAME_IID, v->ax->key_iid[i]) :
                                   sfield_len(FN_ANN_NAME, v->ax->key_len[i]);
    switch (a->kind) {
    case PF_ARG_INT:
        return n + vfield_len(FN_ANN_INT, (uint64_t)a->v.i);
    case PF_ARG_UINT:
        return n + vfield_len(FN_ANN_UINT, a->v.u);
    case PF_ARG_DOUBLE:
        return n + fixed64_len(FN_ANN_DOUBLE);
    }
    return n;
}

static size_t event_body_len(const struct pb_event *v)
{
    size_t n = vfield_len(FN_EVENT_TYPE, v->type);
    if (!v->on_default)
        n += vfield_len(FN_EVENT_TRACK_UUID, v->track);
    if (v->use_iid)
        n += vfield_len(FN_EVENT_NAME_IID, v->iid);
    else if (v->nlen)
        n += sfield_len(FN_EVENT_NAME_STR, v->nlen);
    if (v->has_value)
        n += v->value_is_double ? fixed64_len(FN_EVENT_DOUBLE_COUNTER_VALUE) :
                                  vfield_len(FN_EVENT_COUNTER_VALUE, v->value);
    for (unsigned i = 0; i < v->nargs; i++)
        n += msg_len(FN_EVENT_DEBUG_ANNOTATIONS, v->ax->ann_len[i]);
    return n;
}

/* Writers. Each one's output matches its *_len above byte for byte. */

static void enc_str_entry(struct enc *e, uint64_t iid, const char *s, size_t n)
{
    e_vfield(e, FN_IID, iid);
    e_sfield(e, FN_NAME_STR, s, n);
}

static void enc_name_entry(struct enc *e, const struct pb_event *v)
{
    enc_str_entry(e, v->iid, v->name, v->nlen);
}

/* Writes argument @i's DebugAnnotation. The string value is copied from the
 * caller's buffer here, during the call, so it need not outlive the call. */
static void enc_annotation(struct enc *e, const struct pb_event *v, unsigned i)
{
    const struct pf_arg *a = &v->args[i];
    if (v->ax->key_iid[i])
        e_vfield(e, FN_ANN_NAME_IID, v->ax->key_iid[i]);
    else
        e_sfield(e, FN_ANN_NAME, a->key, v->ax->key_len[i]);
    switch (a->kind) {
    case PF_ARG_INT:
        e_vfield(e, FN_ANN_INT, (uint64_t)a->v.i);
        break;
    case PF_ARG_UINT:
        e_vfield(e, FN_ANN_UINT, a->v.u);
        break;
    case PF_ARG_DOUBLE: {
        uint64_t bits;
        memcpy(&bits, &a->v.d, sizeof(bits));
        e_fixed64(e, FN_ANN_DOUBLE, bits);
        break;
    }
    }
}

static void enc_event_body(struct enc *e, const struct pb_event *v)
{
    e_vfield(e, FN_EVENT_TYPE, v->type);
    if (!v->on_default)
        e_vfield(e, FN_EVENT_TRACK_UUID, v->track);
    if (v->use_iid)
        e_vfield(e, FN_EVENT_NAME_IID, v->iid);
    else if (v->nlen)
        e_name_sfield(e, FN_EVENT_NAME_STR, v->name, v->nlen, v->cut);
    if (v->has_value) {
        if (v->value_is_double)
            e_fixed64(e, FN_EVENT_DOUBLE_COUNTER_VALUE, v->value);
        else
            e_vfield(e, FN_EVENT_COUNTER_VALUE, v->value);
    }
    for (unsigned i = 0; i < v->nargs; i++) {
        e_tag(e, FN_EVENT_DEBUG_ANNOTATIONS, 2);
        e_varint(e, v->ax->ann_len[i]);
        enc_annotation(e, v, i);
    }
}

/* The chunk header's ClockSnapshot body: the trace clock and the incremental
 * clock, both read as @ts. The incremental clock is sequence scoped, so the
 * snapshot is on the writer's own sequence. */
static size_t clock_entry_len(uint32_t id, uint64_t ts, bool incremental)
{
    return vfield_len(FN_CLOCK_ID, id) + vfield_len(FN_CLOCK_TIMESTAMP, ts) +
           (incremental ? vfield_len(FN_CLOCK_IS_INCREMENTAL, 1) : 0);
}

static size_t head_snapshot_len(const struct pb_event *v)
{
    return msg_len(FN_CLOCK_SNAPSHOT_CLOCKS,
                   clock_entry_len(builtin_clock(v->clk), v->head_ts, false)) +
           msg_len(FN_CLOCK_SNAPSHOT_CLOCKS,
                   clock_entry_len(PF_INCREMENTAL_CLOCK_ID, v->head_ts, true));
}

static size_t head_track_defaults_len(const struct pb_event *v)
{
    return vfield_len(FN_TRACK_EVENT_DEFAULTS_TRACK_UUID, v->head_track);
}

/* The clock a chunk's events are on: the incremental clock, tied to the trace
 * clock by the header's snapshot, or the trace clock itself when the header
 * has no snapshot. */
static uint32_t head_clock(const struct pb_event *v)
{
    return v->head_snap ? PF_INCREMENTAL_CLOCK_ID : builtin_clock(v->clk);
}

static size_t head_defaults_len(const struct pb_event *v)
{
    return vfield_len(FN_DEFAULTS_TIMESTAMP_CLOCK_ID, head_clock(v)) +
           msg_len(FN_DEFAULTS_TRACK_EVENT, head_track_defaults_len(v));
}

static size_t head_body_len(const struct pb_event *v)
{
    return vfield_len(FN_PACKET_TRUSTED_SEQ_ID, v->seq_id) +
           vfield_len(FN_PACKET_SEQUENCE_FLAGS,
                      SEQ_INCREMENTAL_STATE_CLEARED | SEQ_NEEDS_INCREMENTAL_STATE) +
           msg_len(FN_PACKET_DEFAULTS, head_defaults_len(v)) +
           (v->head_snap ? msg_len(FN_PACKET_CLOCK_SNAPSHOT, head_snapshot_len(v)) : 0);
}

static void enc_clock_entry(struct enc *e, uint32_t id, uint64_t ts, bool incremental)
{
    e_tag(e, FN_CLOCK_SNAPSHOT_CLOCKS, 2);
    e_varint(e, clock_entry_len(id, ts, incremental));
    e_vfield(e, FN_CLOCK_ID, id);
    e_vfield(e, FN_CLOCK_TIMESTAMP, ts);
    if (incremental)
        e_vfield(e, FN_CLOCK_IS_INCREMENTAL, 1);
}

/* Writes the chunk's header packet. Perfetto reads a packet that needs
 * incremental state only after a clear on its sequence, so every chunk opens
 * with one: the chunk defines every name it uses, and is readable alone. */
static void enc_head(struct enc *e, const struct pb_event *v)
{
    e_tag(e, FN_TRACE_PACKET, 2);
    e_varint(e, v->head_body_len);
    e_vfield(e, FN_PACKET_TRUSTED_SEQ_ID, v->seq_id);
    e_vfield(e, FN_PACKET_SEQUENCE_FLAGS,
             SEQ_INCREMENTAL_STATE_CLEARED | SEQ_NEEDS_INCREMENTAL_STATE);
    e_tag(e, FN_PACKET_DEFAULTS, 2);
    e_varint(e, head_defaults_len(v));
    e_vfield(e, FN_DEFAULTS_TIMESTAMP_CLOCK_ID, head_clock(v));
    e_tag(e, FN_DEFAULTS_TRACK_EVENT, 2);
    e_varint(e, head_track_defaults_len(v));
    e_vfield(e, FN_TRACK_EVENT_DEFAULTS_TRACK_UUID, v->head_track);
    if (!v->head_snap)
        return;
    e_tag(e, FN_PACKET_CLOCK_SNAPSHOT, 2);
    e_varint(e, head_snapshot_len(v));
    enc_clock_entry(e, builtin_clock(v->clk), v->head_ts, false);
    enc_clock_entry(e, PF_INCREMENTAL_CLOCK_ID, v->head_ts, true);
}

/* The event packet. Its timestamp is on the clock the chunk's defaults name:
 * the incremental clock, or the trace clock in a chunk whose header has no
 * snapshot. An @absolute event in an incremental chunk names the trace clock
 * itself. Its track is left out when it is the default. */
static void enc_packet_body(struct enc *e, const struct pb_event *v)
{
    e_vfield(e, FN_PACKET_TIMESTAMP, v->ts);
    if (v->absolute)
        e_vfield(e, FN_PACKET_TIMESTAMP_CLOCK_ID, builtin_clock(v->clk));
    e_vfield(e, FN_PACKET_TRUSTED_SEQ_ID, v->seq_id);
    e_vfield(e, FN_PACKET_SEQUENCE_FLAGS, SEQ_NEEDS_INCREMENTAL_STATE);
    if (v->intern_len) {
        e_tag(e, FN_PACKET_INTERNED_DATA, 2);
        e_varint(e, v->intern_len);
        if (v->define) {
            e_tag(e, FN_INTERNED_EVENT_NAMES, 2);
            e_varint(e, name_entry_len(v));
            enc_name_entry(e, v);
        }
        for (unsigned i = 0; i < v->nargs; i++) {
            if (!v->ax->key_def[i])
                continue;
            e_tag(e, FN_INTERNED_DEBUG_ANNOTATION_NAMES, 2);
            e_varint(e, str_entry_len(v->ax->key_iid[i], v->ax->key_len[i]));
            enc_str_entry(e, v->ax->key_iid[i], v->args[i].key, v->ax->key_len[i]);
        }
    }
    e_tag(e, FN_PACKET_TRACK_EVENT, 2);
    e_varint(e, v->event_len);
    enc_event_body(e, v);
}

/* Sizes @v: sets its argument annotation lengths and body lengths, and returns
 * the length of @v encoded as one Trace.packet, length prefix included, plus
 * the chunk's header packet when @v opens a chunk, without encoding either.
 * The encoder reuses the lengths, so each is computed once per event and
 * chunk. */
static size_t pf_pb_size(struct pb_event *v)
{
    size_t intern = v->define ? msg_len(FN_INTERNED_EVENT_NAMES, name_entry_len(v)) : 0;
    for (unsigned i = 0; i < v->nargs; i++) {
        v->ax->ann_len[i] = ann_body_len(v, i);
        if (v->ax->key_def[i])
            intern += msg_len(FN_INTERNED_DEBUG_ANNOTATION_NAMES,
                              str_entry_len(v->ax->key_iid[i], v->ax->key_len[i]));
    }
    v->intern_len = intern;
    v->event_len = event_body_len(v);
    v->body_len =
        vfield_len(FN_PACKET_TIMESTAMP, v->ts) +
        (v->absolute ? vfield_len(FN_PACKET_TIMESTAMP_CLOCK_ID, builtin_clock(v->clk)) : 0) +
        vfield_len(FN_PACKET_TRUSTED_SEQ_ID, v->seq_id) +
        vfield_len(FN_PACKET_SEQUENCE_FLAGS, SEQ_NEEDS_INCREMENTAL_STATE) +
        (intern ? msg_len(FN_PACKET_INTERNED_DATA, intern) : 0) +
        msg_len(FN_PACKET_TRACK_EVENT, v->event_len);
    size_t size = msg_len(FN_TRACE_PACKET, v->body_len);
    if (v->head) {
        v->head_body_len = head_body_len(v);
        size += msg_len(FN_TRACE_PACKET, v->head_body_len);
    }
    return size;
}

/* Encodes @v, sized by pf_pb_size, into @dst, which must hold the length
 * pf_pb_size returned: the chunk's header packet if @v opens the chunk, then
 * the event as one Trace.packet. Returns the length. */
static size_t pf_pb_encode(uint8_t *dst, const struct pb_event *v)
{
    struct enc e = { dst, 0 };
    if (v->head)
        enc_head(&e, v);
    e_tag(&e, FN_TRACE_PACKET, 2);
    e_varint(&e, v->body_len);
    enc_packet_body(&e, v);
    return e.pos;
}

/* Cache scan over one table of interned names: the event names, or the
 * argument keys. A slot holding @name in the current epoch is used as it is. A
 * slot holding it in an older epoch is redefined here. A new name takes the
 * first slot whose definition is from an older epoch, since a slot defined in
 * this chunk may already be named by a record above it. Slots in @taken are
 * already claimed by this event and are skipped.
 *
 * Each name has a home slot, from a hash of its address, and a hit there costs
 * one compare. A name takes its home slot when that slot is stale, so after
 * the first use it is found there. On a conflict the whole table is scanned,
 * and a name is never held in two slots. */
static unsigned pf_pb_home(const char *name)
{
    /* Multiplicative: literals sit close together in .rodata, so the low
     * address bits alone would put neighbours in the same slot. */
    return (unsigned)(((uint64_t)(uintptr_t)name * 0x9E3779B97F4A7C15ull) >> 59);
}

_Static_assert(PF_PB_INTERN_SLOTS == 32, "pf_pb_home takes 5 bits");

/* Looks up @name in @tab. Returns its slot, or -1 when the table has no slot
 * free for it. Sets *define when the name's definition must go into the
 * current chunk. Read-only, so nothing changes until the record is placed. */
static int pf_pb_lookup(const struct pb_intern_slot *tab, uint32_t epoch, uint32_t taken,
                        const char *name, bool *define)
{
    int home = (int)pf_pb_home(name);
    bool home_free = !(taken & (1u << home));
    if (home_free && tab[home].key == name) {
        *define = tab[home].epoch != epoch;
        return home;
    }
    int stale = home_free && tab[home].epoch != epoch ? home : -1;
    for (int i = 0; i < PF_PB_INTERN_SLOTS; i++) {
        if (taken & (1u << i))
            continue;
        const struct pb_intern_slot *s = &tab[i];
        if (s->key == name) {
            *define = s->epoch != epoch;
            return i;
        }
        if (stale < 0 && s->epoch != epoch)
            stale = i;
    }
    *define = true;
    return stale;
}

/* Records that @slot of @tab now holds @name, of length @nlen, defined in the
 * chunk of @epoch. */
static void pf_pb_commit_name(struct pb_intern_slot *tab, uint32_t epoch, int slot,
                              const char *name, size_t nlen)
{
    tab[slot].key = name;
    tab[slot].len = nlen;
    tab[slot].epoch = epoch;
}

/* Works out what each argument's encoding needs, once per event: the key
 * length. */
static void pf_pb_args_prep(struct pb_event *v, struct pb_args *ax, unsigned nargs,
                            const struct pf_arg *args)
{
    v->nargs = nargs;
    v->args = args;
    v->ax = ax;
    for (unsigned i = 0; i < nargs; i++)
        ax->key_len[i] = strlen(args[i].key);
}

/* Chooses each argument's key for the current chunk. A key already defined in
 * the chunk is sent as its iid. A key not yet defined gets a free slot and is
 * defined in this packet. A key the cache cannot hold is sent inline. A key
 * repeated in this event shares its first argument's entry. Read-only on the
 * writer, like pf_pb_lookup. */
static void pf_pb_resolve_keys(const struct pf_writer *w, const struct pb_event *v,
                               struct pb_args *ax)
{
    uint32_t taken = 0;
    for (unsigned i = 0; i < v->nargs; i++) {
        const char *key = v->args[i].key;
        ax->key_iid[i] = 0;
        ax->key_slot[i] = -1;
        ax->key_def[i] = false;

        unsigned j = 0;
        while (j < i && v->args[j].key != key)
            j++;
        if (j < i) {
            ax->key_iid[i] = ax->key_iid[j];
            continue;
        }

        bool define = false;
        int slot = pf_pb_lookup(w->keys, w->epoch, taken, key, &define);
        if (slot < 0)
            continue;
        taken |= 1u << slot;
        ax->key_slot[i] = slot;
        ax->key_def[i] = define;
        ax->key_iid[i] = (uint64_t)slot + 1;
    }
}

void pf_pb_emit(struct pf_trace *t, struct pf_writer *w, uint64_t ts, uint32_t type, uint64_t track,
                const char *name, bool dynamic, bool has_value, uint64_t raw_value,
                bool value_is_double, unsigned nargs, const struct pf_arg *args)
{
    /* A dynamic name is built by the caller, so it differs per event. It is
     * sent inline and never interned: a pointer key would name the wrong
     * string once the caller reuses its buffer. It is written into the chunk
     * from the caller's buffer, and cut there when it is long. */
    const char *shown = has_value ? NULL : name;
    size_t nlen = 0;
    bool cut = false;
    if (dynamic && shown) {
        nlen = pf_dyn_name_fit(shown, &cut);
        if (cut)
            atomic_fetch_add_explicit(&t->truncated, 1, memory_order_relaxed);
    }

    struct pb_event v;
    /* Not zeroed as a whole: a memset of the struct, now that it carries the
     * argument fields, cost more than the rest of the event. The loop below
     * sets every field the encoder reads; the argument fields need only
     * these initial values. */
    v.nargs = 0;
    v.args = NULL;
    v.ax = NULL;
    /* The macros reject more than PF_MAX_ARGS arguments; a direct caller that
     * passes more gets the first PF_MAX_ARGS. */
    if (nargs > PF_MAX_ARGS)
        nargs = PF_MAX_ARGS;
    struct pb_args ax; /* read only when there are arguments */
    if (nargs)
        pf_pb_args_prep(&v, &ax, nargs, args);

    size_t size;
    int slot;
    bool define;
    for (;;) {
        v.clk = t->cfg.clock_id;
        /* Sequence 1 is the drain's, for the track descriptors and the clock
         * snapshot. Writer i owns sequence i + 2. */
        v.seq_id = w->id + 2;
        /* After a rotation w->cur is a fresh chunk and w->used is 0, so this
         * event opens it, behind a header that makes its track the default
         * and reads the clocks at its time. A writer with no chunk has w->used
         * at the payload, and its first try only rotates, so the header is set
         * up on the try that is encoded.
         *
         * One thread's events reach its chunk in the order they were stamped,
         * so a monotonic clock gives every delta >= 0. REALTIME can step back,
         * and Perfetto handles that in two places:
         *  - A delta cannot be negative. An event below the previous one in an
         *    incremental chunk carries its absolute time on the trace clock,
         *    and names the clock, which overrides the default for that packet.
         *    @last_ts is not moved by it, so the next delta is still from the
         *    last event on the incremental clock.
         *  - Perfetto rejects a snapshot whose trace clock reading is below an
         *    earlier one on the same sequence, and then misplaces the chunk's
         *    events. A chunk that would start below w->head_max gets a header
         *    with no snapshot, whose defaults name the trace clock, and its
         *    events carry absolute times. */
        v.head = w->used == 0;
        bool absolute_chunk = v.head ? ts < w->head_max : w->absolute_chunk;
        v.head_snap = v.head && !absolute_chunk;
        v.absolute = !absolute_chunk && !v.head && ts < w->last_ts;
        v.head_ts = ts;
        v.head_track = track;
        if (absolute_chunk)
            v.ts = ts; /* on the trace clock, which the defaults name */
        else if (v.absolute)
            v.ts = ts;
        else
            v.ts = v.head ? 0 : ts - w->last_ts;
        v.track = track;
        v.on_default = v.head || track == w->track;
        v.type = type;
        v.has_value = has_value;
        v.value_is_double = value_is_double;
        v.value = raw_value;

        slot = -1;
        define = false;
        if (!dynamic && shown && shown[0])
            slot = pf_pb_lookup(w->intern, w->epoch, 0, shown, &define);
        /* A hit carries the name's length from when it was defined. */
        if (!dynamic && shown)
            nlen = (slot >= 0 && w->intern[slot].key == shown) ? w->intern[slot].len :
                                                                 strlen(shown);
        v.use_iid = slot >= 0;
        v.define = v.use_iid && define;
        v.iid = (uint64_t)slot + 1;
        v.name = shown;
        v.nlen = nlen;
        v.cut = cut;
        if (nargs)
            pf_pb_resolve_keys(w, &v, &ax);

        size = pf_pb_size(&v);
        if (size > t->chunk_payload) {
            /* Too big for any chunk; a record is never split. */
            pf_writer_drop(w);
            return;
        }
        if (w->cur && w->used + size <= t->chunk_payload)
            break;
        /* Rotation changes the epoch, so the names are looked up again. */
        if (!pf_writer_rotate(t, w))
            return; /* no chunk free; pf_writer_rotate counted the drop */
    }

    if (v.define)
        pf_pb_commit_name(w->intern, w->epoch, slot, shown, nlen);
    for (unsigned i = 0; i < nargs; i++) {
        if (ax.key_def[i])
            pf_pb_commit_name(w->keys, w->epoch, ax.key_slot[i], args[i].key, ax.key_len[i]);
    }
    pf_pb_encode(w->cur->bytes + w->used, &v);
    w->used += (uint32_t)size;
    if (v.head) {
        w->track = track;
        w->absolute_chunk = !v.head_snap;
        if (v.head_snap)
            w->head_max = ts;
    }
    if (v.head_snap || (!w->absolute_chunk && !v.absolute))
        w->last_ts = ts;

    atomic_store_explicit(&w->cur->used, w->used, memory_order_release);
    atomic_store_explicit(&w->events, atomic_load_explicit(&w->events, memory_order_relaxed) + 1,
                          memory_order_relaxed);
}
