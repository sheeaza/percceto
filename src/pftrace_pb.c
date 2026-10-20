#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pb_writer.h"
#include "pftrace_internal.h"

/* --- Confirmed Perfetto wire-format field numbers -------------------
 * Pulled directly from the compiled perfetto_trace_pb2 DESCRIPTOR (the
 * official schema), not from memory. See plan doc for the full table. */

/* Trace */
#define FN_TRACE_PACKET 1

/* TracePacket */
#define FN_PACKET_TIMESTAMP 8
#define FN_PACKET_TIMESTAMP_CLOCK_ID 58
#define FN_PACKET_TRUSTED_SEQ_ID 10
#define FN_PACKET_TRACK_EVENT 11
#define FN_PACKET_INTERNED_DATA 12
#define FN_PACKET_SEQUENCE_FLAGS 13
#define FN_PACKET_TRACK_DESCRIPTOR 60
#define FN_PACKET_CLOCK_SNAPSHOT 6
#define FN_CLOCK_SNAPSHOT_CLOCKS 1
#define FN_CLOCK_SNAPSHOT_PRIMARY 2
#define FN_CLOCK_ID 1
#define FN_CLOCK_TIMESTAMP 2

/* BuiltinClock ids from ClockSnapshot; the trace clock is always one of
 * these, see pf_trace_open(). */
#define BUILTIN_CLOCK_REALTIME 1
#define BUILTIN_CLOCK_MONOTONIC 3
#define BUILTIN_CLOCK_BOOTTIME 6

static uint32_t builtin_clock(clockid_t clk)
{
    if (clk == CLOCK_REALTIME)
        return BUILTIN_CLOCK_REALTIME;
    if (clk == CLOCK_MONOTONIC)
        return BUILTIN_CLOCK_MONOTONIC;
    return BUILTIN_CLOCK_BOOTTIME;
}

/* Writes a packet's timestamp with its clock. Perfetto reads a packet with no
 * timestamp_clock_id as BOOTTIME, so a REALTIME or MONOTONIC reading must name
 * its clock or the packet cannot be related to the trace clock. */
static void put_packet_timestamp(struct pb_buf *packet, clockid_t clk,
                                 uint64_t ts)
{
    pb_put_varint_field(packet, FN_PACKET_TIMESTAMP, ts);
    pb_put_varint_field(packet, FN_PACKET_TIMESTAMP_CLOCK_ID,
                        builtin_clock(clk));
}

#define SEQ_INCREMENTAL_STATE_CLEARED 1u
#define SEQ_NEEDS_INCREMENTAL_STATE 2u

/* TrackEvent */
#define FN_EVENT_TYPE 9
#define FN_EVENT_NAME_IID 10
#define FN_EVENT_TRACK_UUID 11
#define FN_EVENT_COUNTER_VALUE 30
#define FN_EVENT_NAME_STR 23
#define FN_EVENT_DOUBLE_COUNTER_VALUE 44

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

/* InternedData / EventName */
#define FN_INTERNED_EVENT_NAMES 2
#define FN_IID 1
#define FN_NAME_STR 2

/* Frames `packet` as one entry of the (implicit, streamed) Trace.packet
 * repeated field and stages it for writing. */
static int stage_packet(struct pf_trace *t, const struct pb_buf *packet)
{
    pb_put_tag(&t->out, FN_TRACE_PACKET, 2);
    pb_put_varint(&t->out, packet->len);
    pb_put_bytes(&t->out, packet->data, packet->len);
    if (t->out.len >= PF_OUT_FLUSH_THRESHOLD)
        return out_flush(t);
    return 0;
}

static void add_interned_entry(struct pb_buf *interned, uint32_t field_no,
                               uint64_t iid, const char *name, size_t len,
                               struct pb_buf *entry)
{
    pb_buf_reset(entry);
    pb_put_varint_field(entry, FN_IID, iid);
    pb_put_string_field(entry, FN_NAME_STR, name, len);
    pb_put_submessage_field(interned, field_no, entry);
}

/* --- Track descriptor emission ----------------------------------------- */

static int emit_track(struct pf_trace *t, const struct track_def *td,
                      uint32_t extra_flags)
{
    struct pb_buf *inner = &t->pb.scratch_a;
    struct pb_buf *desc = &t->pb.scratch_b;
    struct pb_buf *packet = &t->pb.scratch_c;

    pb_buf_reset(inner);
    pb_buf_reset(desc);
    pb_buf_reset(packet);

    pb_put_varint_field(desc, FN_TRACK_UUID, td->uuid);
    if (td->parent_uuid)
        pb_put_varint_field(desc, FN_TRACK_PARENT_UUID, td->parent_uuid);

    switch (td->kind) {
    case TRACK_PROCESS:
        pb_put_varint_field(inner, FN_PROCESS_PID, (uint64_t)(uint32_t)td->pid);
        if (td->has_name)
            pb_put_string_field(inner, FN_PROCESS_NAME, td->name,
                                strlen(td->name));
        pb_put_submessage_field(desc, FN_TRACK_PROCESS, inner);
        break;
    case TRACK_THREAD:
        pb_put_varint_field(inner, FN_THREAD_PID, (uint64_t)(uint32_t)td->pid);
        pb_put_varint_field(inner, FN_THREAD_TID, (uint64_t)(uint32_t)td->tid);
        if (td->has_name)
            pb_put_string_field(inner, FN_THREAD_NAME, td->name,
                                strlen(td->name));
        pb_put_submessage_field(desc, FN_TRACK_THREAD, inner);
        break;
    case TRACK_COUNTER:
        if (td->unit != PF_UNIT_UNSPECIFIED)
            pb_put_varint_field(inner, FN_COUNTER_UNIT, (uint64_t)td->unit);
        if (td->has_name)
            pb_put_string_field(desc, FN_TRACK_NAME, td->name,
                                strlen(td->name));
        pb_put_submessage_field(desc, FN_TRACK_COUNTER, inner);
        break;
    }

    pb_put_varint_field(packet, FN_PACKET_TRUSTED_SEQ_ID, 1);
    if (extra_flags)
        pb_put_varint_field(packet, FN_PACKET_SEQUENCE_FLAGS, extra_flags);
    pb_put_submessage_field(packet, FN_PACKET_TRACK_DESCRIPTOR, desc);

    return stage_packet(t, packet);
}

/* Adds one Clock entry, a builtin id and a reading, to the snapshot. */
static void put_clock_entry(struct pb_buf *snap, struct pb_buf *clock,
                            uint32_t id, uint64_t ts)
{
    pb_buf_reset(clock);
    pb_put_varint_field(clock, FN_CLOCK_ID, id);
    pb_put_varint_field(clock, FN_CLOCK_TIMESTAMP, ts);
    pb_put_submessage_field(snap, FN_CLOCK_SNAPSHOT_CLOCKS, clock);
}

/* Stages a ClockSnapshot naming the trace clock as primary. Its REALTIME
 * reading, taken back to back with the trace clock's, lets Perfetto map the
 * timestamps to wall time. A BOOTTIME reading is added too: trace_processor
 * always links BOOTTIME to the trace clock for a proto trace, and a snapshot
 * with no BOOTTIME entry leaves that link missing and reports it. Caller holds
 * out_lock. */
static int emit_clock_snapshot(struct pf_trace *t)
{
    clockid_t clk = t->cfg.clock_id;
    struct pb_buf *clock = &t->pb.scratch_a;
    struct pb_buf *snap = &t->pb.scratch_b;
    struct pb_buf *packet = &t->pb.scratch_c;

    uint64_t trace_ts = now_ns(clk);
    uint64_t real_ts = now_ns(CLOCK_REALTIME);

    pb_buf_reset(snap);
    pb_buf_reset(packet);
    put_clock_entry(snap, clock, builtin_clock(clk), trace_ts);
    if (clk != CLOCK_REALTIME)
        put_clock_entry(snap, clock, BUILTIN_CLOCK_REALTIME, real_ts);
    if (clk != CLOCK_BOOTTIME)
        put_clock_entry(snap, clock, BUILTIN_CLOCK_BOOTTIME,
                        now_ns(CLOCK_BOOTTIME));
    pb_put_varint_field(snap, FN_CLOCK_SNAPSHOT_PRIMARY, builtin_clock(clk));

    pb_put_varint_field(packet, FN_PACKET_TRUSTED_SEQ_ID, 1);
    pb_put_submessage_field(packet, FN_PACKET_CLOCK_SNAPSHOT, snap);
    return stage_packet(t, packet);
}

/* Emits whatever track descriptors the output file still needs.
 *
 * `restart` resets the sequence's incremental state, which is what makes
 * a snapshot dump self-contained: interning restarts from scratch and
 * every track is re-declared, so the appended window can be read without
 * reference to anything written earlier. Continuous mode passes false
 * after the first drain and emits only newly registered tracks. A restart
 * also writes the ClockSnapshot, so every dump names its own clock.
 *
 * Caller holds out_lock. */
static int emit_track_header(struct pf_trace *t, bool restart)
{
    pthread_mutex_lock(&t->track_lock);
    unsigned count = t->track_count;
    pthread_mutex_unlock(&t->track_lock);

    unsigned first = restart ? 0 : t->tracks_emitted;
    if (restart) {
        intern_table_reset(&t->name_table);
        if (emit_clock_snapshot(t) != 0)
            return -1;
    }

    for (unsigned i = 0; i < count; i++) {
        struct track_def td;
        if (!track_emit_due(t, i, first, &td))
            continue;
        /* The flag belongs on the first packet of the restarted
         * sequence; later packets in the same dump must not repeat it. */
        uint32_t flags =
            (restart && i == first) ? SEQ_INCREMENTAL_STATE_CLEARED : 0u;
        if (emit_track(t, &td, flags) != 0)
            return -1;
    }

    /* A restart with no tracks at all still has to clear state, or the
     * reader keeps stale interning from the previous window. */
    if (restart && count == 0) {
        pb_buf_reset(&t->pb.scratch_c);
        put_packet_timestamp(&t->pb.scratch_c, t->cfg.clock_id,
                             now_ns(t->cfg.clock_id));
        pb_put_varint_field(&t->pb.scratch_c, FN_PACKET_TRUSTED_SEQ_ID, 1);
        pb_put_varint_field(&t->pb.scratch_c, FN_PACKET_SEQUENCE_FLAGS,
                            SEQ_INCREMENTAL_STATE_CLEARED);
        if (stage_packet(t, &t->pb.scratch_c) != 0)
            return -1;
    }

    t->tracks_emitted = count;
    return 0;
}

/* --- Record -> packet -------------------------------------------------- */

/* One parsed conversion, "%[flags][width][.prec][len]conv". Accepts only
 * what the Python converter accepts too, so both writers agree on every
 * name: flags - + space 0, width and precision of up to three digits, a
 * length of l or ll, and the conversions in parse_spec(). */
struct pf_spec {
    const char *start; /* the '%' */
    size_t len; /* bytes from the '%' through the conversion char */
    size_t body; /* bytes before the length modifier */
    char conv;
    bool minus, plus, space, zero;
    int width; /* -1 if none */
    int prec; /* -1 if none */
    int lenmod; /* 0 none, 1 "l", 2 "ll" */
};

/* Parses the spec whose '%' is at @start. Returns false for anything it
 * does not accept, including '#'. C and Python render %#o and %+u
 * differently, so neither accepts them. */
static bool parse_spec(const char *start, struct pf_spec *s)
{
    const char *p = start + 1;
    memset(s, 0, sizeof *s);
    s->start = start;
    s->width = -1;
    s->prec = -1;

    for (;; p++) {
        if (*p == '-')
            s->minus = true;
        else if (*p == '+')
            s->plus = true;
        else if (*p == ' ')
            s->space = true;
        else if (*p == '0')
            s->zero = true;
        else
            break;
    }

    int digits = 0;
    int n = 0;
    while (*p >= '0' && *p <= '9') {
        if (++digits > 3)
            return false;
        n = n * 10 + (*p++ - '0');
    }
    if (digits)
        s->width = n;

    if (*p == '.') {
        p++;
        digits = 0;
        n = 0;
        while (*p >= '0' && *p <= '9') {
            if (++digits > 3)
                return false;
            n = n * 10 + (*p++ - '0');
        }
        if (digits == 0)
            return false;
        s->prec = n;
    }

    s->body = (size_t)(p - start);
    if (*p == 'l') {
        s->lenmod = 1;
        p++;
        if (*p == 'l') {
            s->lenmod = 2;
            p++;
        }
    }
    s->conv = *p;
    if (!s->conv)
        return false;
    s->len = (size_t)(p + 1 - start);

    switch (s->conv) {
    case 'd':
    case 'i':
        return true;
    case 'u':
    case 'x':
    case 'X':
    case 'o':
        return !s->plus && !s->space;
    case 'c':
    case 'p':
        return !s->plus && !s->space && !s->zero && s->prec < 0 &&
               s->lenmod == 0;
    case 'f':
    case 'F':
    case 'e':
    case 'E':
    case 'g':
    case 'G':
        return s->lenmod == 0;
    default:
        return false;
    }
}

/* Renders one argument for @s into @dst, which is @room bytes. Returns the
 * number of characters the result needs, or -1 if the argument's type does
 * not suit the conversion. A result of @room or more is too long; the
 * caller treats it as a failure. */
static int render_arg(char *dst, size_t room, const struct pf_spec *s,
                      uint64_t bits, bool is_double)
{
    char fs[40];
    double d;

    switch (s->conv) {
    case 'd':
    case 'i':
    case 'u':
    case 'x':
    case 'X':
    case 'o':
        if (is_double)
            return -1;
        /* Ints always go through long long, so the spec is rewritten with
         * "ll" whatever length it had. */
        memcpy(fs, s->start, s->body);
        fs[s->body] = 'l';
        fs[s->body + 1] = 'l';
        fs[s->body + 2] = s->conv;
        fs[s->body + 3] = '\0';
        if (s->conv == 'd' || s->conv == 'i')
            return snprintf(dst, room, fs, (long long)bits);
        return snprintf(dst, room, fs, (unsigned long long)bits);

    case 'c':
        /* One byte, and only 7-bit ASCII, so the output is valid UTF-8 and
         * matches the Python side. */
        if (is_double || bits < 1 || bits > 127)
            return -1;
        memcpy(fs, s->start, s->body);
        fs[s->body] = 'c';
        fs[s->body + 1] = '\0';
        return snprintf(dst, room, fs, (int)bits);

    case 'p': {
        /* Hand-formatted rather than %p, whose text is libc's choice. */
        if (is_double)
            return -1;
        char hex[32];
        int n = snprintf(hex, sizeof hex, "0x%llx", (unsigned long long)bits);
        int total = s->width > n ? s->width : n;
        if ((size_t)total >= room)
            return total;
        int lead = s->minus ? 0 : total - n;
        int o = 0;
        for (int i = 0; i < lead; i++)
            dst[o++] = ' ';
        memcpy(dst + o, hex, (size_t)n);
        o += n;
        for (int i = 0; i < total - n - lead; i++)
            dst[o++] = ' ';
        dst[o] = '\0';
        return total;
    }

    case 'f':
    case 'F':
    case 'e':
    case 'E':
    case 'g':
    case 'G':
        if (!is_double)
            return -1;
        memcpy(&d, &bits, sizeof d);
        memcpy(fs, s->start, s->len);
        fs[s->len] = '\0';
        return snprintf(dst, room, fs, d);

    default:
        return -1;
    }
}

/* Expands @fmt with the event's args into @out, which is @cap bytes. Returns
 * false if the format does not match the args, or the result does not fit.
 * The caller then falls back to the bare format string. */
static bool format_name(char *out, size_t cap, const char *fmt,
                        const struct pf_rec *r, unsigned nargs)
{
    size_t o = 0;
    unsigned k = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            if (o + 1 >= cap)
                return false;
            out[o++] = *p;
            continue;
        }
        if (p[1] == '%') {
            if (o + 1 >= cap)
                return false;
            out[o++] = '%';
            p++;
            continue;
        }
        struct pf_spec s;
        if (!parse_spec(p, &s) || k >= nargs)
            return false;
        uint32_t is_dbl_bit = k == 0 ? RF_ARG0_IS_DOUBLE : RF_ARG1_IS_DOUBLE;
        bool is_double = (r->flags & is_dbl_bit) != 0;
        int n = render_arg(out + o, cap - o, &s, r->arg[k], is_double);
        if (n < 0 || (size_t)n >= cap - o)
            return false;
        o += (size_t)n;
        k++;
        p += s.len - 1;
    }
    if (k != nargs)
        return false;
    out[o] = '\0';
    return true;
}

/* Caller holds out_lock. */
static int emit_record(struct pf_trace *t, const struct pf_rec *r)
{
    uint64_t ts = r->ts;
    uint64_t track_uuid = r->track;
    uint32_t type = r->type;
    uint32_t flags = r->flags;

    bool has_value = (flags & RF_HAS_VALUE) != 0;
    uint64_t raw_value = has_value ? r->u.value : 0;
    /* Only dereferenced here, on the drain thread -- the hot path stored
     * the address and nothing else. */
    const char *name = has_value ? NULL : r->u.name;
    unsigned nargs = (flags & RF_NARGS_MASK) >> RF_NARGS_SHIFT;

    /* An event with args gets a name built per event, from its format and
     * its values. That name is unbounded, so it is written inline and not
     * interned: the intern table stays keyed on the literal formats only. */
    char formatted[PF_FORMAT_NAME_MAX];
    bool dynamic = nargs > 0 && name != NULL;
    if (dynamic && format_name(formatted, sizeof formatted, name, r, nargs))
        name = formatted;
    size_t nlen = name ? strlen(name) : 0;

    struct pb_buf *interned = &t->pb.scratch_a;
    struct pb_buf *ev = &t->pb.scratch_b;
    struct pb_buf *packet = &t->pb.scratch_c;
    struct pb_buf *entry = &t->pb.scratch_entry;
    pb_buf_reset(interned);
    pb_buf_reset(ev);
    pb_buf_reset(packet);

    bool have_interned = false;
    uint64_t name_iid = 0;
    bool name_interned = false;
    if (nlen && !dynamic) {
        bool is_new = false;
        name_interned =
            intern_lookup_or_insert(&t->name_table, name, &name_iid, &is_new);
        if (name_interned && is_new) {
            add_interned_entry(interned, FN_INTERNED_EVENT_NAMES, name_iid,
                               name, nlen, entry);
            have_interned = true;
        }
    }

    pb_put_varint_field(ev, FN_EVENT_TYPE, type);
    pb_put_varint_field(ev, FN_EVENT_TRACK_UUID, track_uuid);
    if (nlen) {
        if (name_interned)
            pb_put_varint_field(ev, FN_EVENT_NAME_IID, name_iid);
        else
            pb_put_string_field(ev, FN_EVENT_NAME_STR, name, nlen);
    }
    if (has_value) {
        if (flags & RF_VALUE_IS_DOUBLE)
            pb_put_fixed64_field(ev, FN_EVENT_DOUBLE_COUNTER_VALUE, raw_value);
        else
            pb_put_varint_field(ev, FN_EVENT_COUNTER_VALUE, raw_value);
    }

    put_packet_timestamp(packet, t->cfg.clock_id, ts);
    pb_put_varint_field(packet, FN_PACKET_TRUSTED_SEQ_ID, 1);
    pb_put_varint_field(packet, FN_PACKET_SEQUENCE_FLAGS,
                        SEQ_NEEDS_INCREMENTAL_STATE);
    if (have_interned)
        pb_put_submessage_field(packet, FN_PACKET_INTERNED_DATA, interned);
    pb_put_submessage_field(packet, FN_PACKET_TRACK_EVENT, ev);

    int rc = stage_packet(t, packet);
    return rc;
}

/* Drains the committed chunks as protobuf packets. Caller holds out_lock. */
int pb_drain_locked(struct pf_trace *t, const struct pf_chunk *head)
{
    bool restart = !t->header_written || t->cfg.mode == PF_MODE_SNAPSHOT;
    if (emit_track_header(t, restart) != 0)
        return -1;
    t->header_written = true;

    struct pf_walk w;
    pf_walk_begin(&w, head);

    const struct pf_rec *rec;
    while ((rec = pf_walk_next(&w)) != NULL) {
        if (emit_record(t, rec) != 0)
            return -1;
    }

    return out_flush(t);
}
