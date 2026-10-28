/* gettid() and pthread_getname_np() are GNU extensions. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "pf_pb_encode.h"
#include "pftrace_internal.h"

/* --- Event emission (hot path) ------------------------------------------
 * Write one record into the calling thread's chunk. No shared state is read
 * or written here, and the only branch that is not the common case is the
 * rotation once per chunk. See docs/chunk_buffers_design.md.
 *
 * Each entry point takes the stages its kind needs: emit_start, then the
 * protobuf encoder or the raw stages below.
 *
 * A static name is stored by address, and the drain reads its characters
 * later. A dynamic name is copied into the record now, since the caller's
 * buffer may be reused before the drain runs. */

/* Stamps the event and finds the calling thread's writer. Returns NULL, and
 * counts the event, when the thread has no writer.
 *
 * Timestamp before anything else, so the cost of finding a chunk lands
 * between this event and the next rather than inside this one. Chunks are per
 * thread, so records reach the drain in commit order, which is not timestamp
 * order across threads. Perfetto sorts by timestamp. */
static struct pf_writer *emit_start(struct pf_trace *t, uint64_t *ts)
{
    *ts = now_ns(t->cfg.clock_id);
    struct pf_writer *w = pf_writer_for(t);
    if (!w)
        atomic_fetch_add_explicit(&t->overflow_dropped, 1, memory_order_relaxed);
    return w;
}

/* Returns room for a raw record of @rec_size bytes in @w's chunk, rotating
 * when the rest of the chunk is too small, or NULL when the record is dropped.
 * Records are never split across chunks, so the drain can walk each chunk on
 * its own. A record larger than a whole
 * chunk payload cannot be stored at all, so it is dropped and counted. */
static struct pf_rec *raw_reserve(struct pf_trace *t, struct pf_writer *w, uint32_t rec_size)
{
    if (rec_size > t->chunk_payload) {
        pf_writer_drop(w);
        return NULL;
    }
    if (w->used + rec_size > t->chunk_payload && !pf_writer_rotate(t, w))
        return NULL; /* no chunk free; pf_writer_rotate counted the drop */
    return (struct pf_rec *)(w->cur->bytes + w->used);
}

static void raw_head(struct pf_rec *r, uint64_t ts, uint64_t track, uint32_t type, uint32_t flags)
{
    r->ts = ts;
    r->track = track;
    r->type = type;
    r->flags = flags;
}

/* The argument section's size, for at least one argument. The macros reject
 * more than PF_MAX_ARGS arguments; a direct caller that passes more gets the
 * first PF_MAX_ARGS. */
static uint32_t raw_args_bytes(unsigned *nargs)
{
    if (*nargs > PF_MAX_ARGS)
        *nargs = PF_MAX_ARGS;
    return PF_REC_ARGS_HEAD + *nargs * PF_REC_ARG_BYTES;
}

/* Writes the argument section after @r's head: the count, 4 reserved bytes,
 * then one entry per argument: the key's pointer, its kind, 4 reserved bytes,
 * and the value's 8 bytes. Keys are literals, so the pointer is what the drain
 * interns. */
static void raw_put_args(struct pf_rec *r, unsigned nargs, const struct pf_arg *args)
{
    uint32_t *hdr = (uint32_t *)((char *)r + PF_REC_HEAD_BYTES);
    hdr[0] = nargs;
    hdr[1] = 0;
    for (unsigned i = 0; i < nargs; i++) {
        char *e = (char *)hdr + PF_REC_ARGS_HEAD + i * PF_REC_ARG_BYTES;
        uint64_t key = (uint64_t)(uintptr_t)args[i].key;
        uint32_t kind = (uint32_t)args[i].kind;
        uint32_t zero = 0;
        memcpy(e, &key, sizeof(key));
        memcpy(e + 8, &kind, sizeof(kind));
        memcpy(e + 12, &zero, sizeof(zero));
        memcpy(e + 16, &args[i].v, sizeof(args[i].v));
    }
}

/* Publishes the record after it is complete. */
static void raw_commit(struct pf_writer *w, uint32_t rec_size)
{
    w->used += rec_size;
    pf_writer_publish(w);
}

/* Writes a raw record that is the head alone: no arguments, no copied name.
 * @u fills the name/value union: a borrowed name, a counter value, or 0. */
static void raw_emit_head(struct pf_trace *t, struct pf_writer *w, uint64_t ts, uint64_t track,
                          uint32_t type, uint32_t flags, uint64_t u)
{
    struct pf_rec *r = raw_reserve(t, w, PF_REC_HEAD_BYTES);
    if (!r)
        return;
    raw_head(r, ts, track, type, flags);
    r->u.value = u;
    raw_commit(w, PF_REC_HEAD_BYTES);
}

/* A counter sample: a value and no name, in the head alone. */
static void emit_counter(struct pf_trace *t, uint64_t track_uuid, uint64_t raw_value,
                         bool value_is_double)
{
    uint64_t ts;
    struct pf_writer *w = emit_start(t, &ts);
    if (!w)
        return;
    if (t->cfg.format == PF_FORMAT_PROTOBUF) {
        pf_pb_counter(t, w, ts, track_uuid, raw_value, value_is_double);
        return;
    }

    raw_emit_head(t, w, ts, track_uuid, TYPE_COUNTER,
                  RF_HAS_VALUE | (value_is_double ? RF_VALUE_IS_DOUBLE : 0u), raw_value);
}

/* An event with a static name, in the head alone. */
static void emit_named(struct pf_trace *t, uint32_t type, uint64_t track_uuid, const char *name)
{
    uint64_t ts;
    struct pf_writer *w = emit_start(t, &ts);
    if (!w)
        return;
    if (t->cfg.format == PF_FORMAT_PROTOBUF) {
        pf_pb_named(t, w, ts, type, track_uuid, name);
        return;
    }

    raw_emit_head(t, w, ts, track_uuid, type, 0u, (uint64_t)(uintptr_t)name);
}

/* An event with a static name and 1 to PF_MAX_ARGS typed arguments, as
 * pf_slice_begin_args_impl() documents. The raw record carries them in a
 * section after the head. */
static void emit_named_args(struct pf_trace *t, uint32_t type, uint64_t track_uuid,
                            const char *name, unsigned nargs, const struct pf_arg *args)
{
    uint64_t ts;
    struct pf_writer *w = emit_start(t, &ts);
    if (!w)
        return;
    if (t->cfg.format == PF_FORMAT_PROTOBUF) {
        pf_pb_named_args(t, w, ts, type, track_uuid, name, nargs, args);
        return;
    }

    uint32_t rec_size = PF_REC_HEAD_BYTES + raw_args_bytes(&nargs);
    struct pf_rec *r = raw_reserve(t, w, rec_size);
    if (!r)
        return;
    raw_head(r, ts, track_uuid, type, RF_HAS_ARGS);
    r->u.name = name;
    raw_put_args(r, nargs, args);
    raw_commit(w, rec_size);
}

/* An event with a name the caller built. The raw record copies it after the
 * head, capped at PF_DYN_NAME_MAX. */
static void emit_dyn(struct pf_trace *t, uint32_t type, uint64_t track_uuid, const char *name)
{
    uint64_t ts;
    struct pf_writer *w = emit_start(t, &ts);
    if (!w)
        return;
    if (t->cfg.format == PF_FORMAT_PROTOBUF) {
        pf_pb_dyn(t, w, ts, type, track_uuid, name);
        return;
    }

    if (!name) {
        raw_emit_head(t, w, ts, track_uuid, type, 0u, 0);
        return;
    }
    bool cut;
    uint32_t len = (uint32_t)pf_dyn_name_fit(name, &cut);
    if (cut)
        atomic_fetch_add_explicit(&t->truncated, 1, memory_order_relaxed);
    uint32_t padded = roundup(len, PF_REC_ALIGN);
    uint32_t rec_size = PF_REC_HEAD_BYTES + padded;

    struct pf_rec *r = raw_reserve(t, w, rec_size);
    if (!r)
        return;
    raw_head(r, ts, track_uuid, type, RF_NAME_COPIED);
    r->u.name_len = len;
    char *body = (char *)r + PF_REC_HEAD_BYTES;
    pf_dyn_name_copy(body, name, len, cut);
    memset(body + len, 0, padded - len);
    raw_commit(w, rec_size);
}

/* A slice end: no name, no value, in the head alone. */
static void emit_end(struct pf_trace *t, uint64_t track_uuid)
{
    uint64_t ts;
    struct pf_writer *w = emit_start(t, &ts);
    if (!w)
        return;
    if (t->cfg.format == PF_FORMAT_PROTOBUF) {
        pf_pb_end(t, w, ts, track_uuid);
        return;
    }

    raw_emit_head(t, w, ts, track_uuid, TYPE_SLICE_END, 0u, 0);
}

void pf_slice_begin_args_impl(struct pf_trace *t, uint64_t track_uuid, const char *name,
                              unsigned nargs, const struct pf_arg *args)
{
    emit_named_args(t, TYPE_SLICE_BEGIN, track_uuid, name, nargs, args);
}

void pf_instant_event_args_impl(struct pf_trace *t, uint64_t track_uuid, const char *name,
                                unsigned nargs, const struct pf_arg *args)
{
    emit_named_args(t, TYPE_INSTANT, track_uuid, name, nargs, args);
}

void pf_slice_begin_impl(struct pf_trace *t, uint64_t track_uuid, const char *name)
{
    emit_named(t, TYPE_SLICE_BEGIN, track_uuid, name);
}

void pf_slice_begin_dyn_impl(struct pf_trace *t, uint64_t track_uuid, const char *name)
{
    emit_dyn(t, TYPE_SLICE_BEGIN, track_uuid, name);
}

/* The formatter's buffer: PF_DYN_NAME_MAX bytes, one more so that a name too
 * long for the cap still reads as too long and is cut with "...", and the NUL. */
#define FMT_NAME_BYTES (PF_DYN_NAME_MAX + 2)

/* Formats @fmt into @buf and returns @buf. A failed format leaves an empty name. */
static const char *format_name(char *buf, size_t size, const char *fmt, va_list ap)
{
    if (vsnprintf(buf, size, fmt, ap) < 0)
        buf[0] = '\0';
    return buf;
}

void pf_slice_begin_fmt_impl(struct pf_trace *t, uint64_t track_uuid, const char *fmt, ...)
{
    char name[FMT_NAME_BYTES];
    va_list ap;
    va_start(ap, fmt);
    emit_dyn(t, TYPE_SLICE_BEGIN, track_uuid, format_name(name, sizeof(name), fmt, ap));
    va_end(ap);
}

void pf_instant_event_fmt_impl(struct pf_trace *t, uint64_t track_uuid, const char *fmt, ...)
{
    char name[FMT_NAME_BYTES];
    va_list ap;
    va_start(ap, fmt);
    emit_dyn(t, TYPE_INSTANT, track_uuid, format_name(name, sizeof(name), fmt, ap));
    va_end(ap);
}

void pf_instant_event_impl(struct pf_trace *t, uint64_t track_uuid, const char *name)
{
    emit_named(t, TYPE_INSTANT, track_uuid, name);
}

void pf_instant_event_dyn_impl(struct pf_trace *t, uint64_t track_uuid, const char *name)
{
    emit_dyn(t, TYPE_INSTANT, track_uuid, name);
}

void pf_slice_end(struct pf_trace *t, uint64_t track_uuid)
{
    emit_end(t, track_uuid);
}

void pf_counter_set_int(struct pf_trace *t, uint64_t counter_track_uuid, int64_t value)
{
    emit_counter(t, counter_track_uuid, (uint64_t)value, false);
}

void pf_counter_set_double(struct pf_trace *t, uint64_t counter_track_uuid, double value)
{
    uint64_t bits;
    memcpy(&bits, &value, 8);
    emit_counter(t, counter_track_uuid, bits, true);
}

/* --- Simple API -------------------------------------------------------
 * The trace is the process default. pf_trace_open_g() in pftrace.c sets it and
 * pf_trace_close_g() clears it. pf_trace_open() and pf_trace_close() never
 * touch it. It is expected to be created once by the main thread
 * and live for the whole process, so it is never swapped while threads
 * use it. Each thread caches the uuid of its own track in its pf_tls slot for
 * that trace. The hot path is one pointer load, one TLS read and a flag check.
 * Syscalls (gettid, getpid) run once per thread per trace, on first use.
 *
 * Renames are kept in the slot, so a name set before registration applies
 * at registration, and one set after re-emits the descriptor. */

/* Cache miss: register this thread's track on @t. Name precedence: the
 * name set by pf_thread_name(), then the OS thread name, then "thread-<tid>".
 * Binds the thread to @t first, so the track lives in the slot that holds
 * the thread's writer. */
static uint64_t thread_track_register(struct pf_trace *t)
{
    char os_name[PF_TRACK_NAME_MAX];
    char fallback[32];
    const char *name = pf_tls.has_name ? pf_tls.name : NULL;
    int32_t tid = (int32_t)gettid();

    pf_writer_for(t);

    if (!name && pthread_getname_np(pthread_self(), os_name, sizeof(os_name)) == 0 && os_name[0])
        name = os_name;
    if (!name) {
        snprintf(fallback, sizeof(fallback), "thread-%d", tid);
        name = fallback;
    }

    struct pf_tls_slot *s = &pf_tls.slots[t->index];
    s->track_uuid = track_add(t, TRACK_THREAD, t->process_uuid, (int32_t)getpid(), tid, name,
                              PF_UNIT_UNSPECIFIED);
    return s->track_uuid;
}

static uint64_t thread_track(struct pf_trace *t)
{
    struct pf_tls_slot *s = &pf_tls.slots[t->index];
    if (s->trace == t && s->track_uuid)
        return s->track_uuid;
    return thread_track_register(t);
}

void pf_slice_begin_g_impl(const char *name)
{
    struct pf_trace *t = default_trace();
    if (t)
        emit_named(t, TYPE_SLICE_BEGIN, thread_track(t), name);
}

void pf_slice_end_g(void)
{
    struct pf_trace *t = default_trace();
    if (t)
        emit_end(t, thread_track(t));
}

void pf_instant_event_g_impl(const char *name)
{
    struct pf_trace *t = default_trace();
    if (t)
        emit_named(t, TYPE_INSTANT, thread_track(t), name);
}

void pf_slice_begin_args_g_impl(const char *name, unsigned nargs, const struct pf_arg *args)
{
    struct pf_trace *t = default_trace();
    if (t)
        emit_named_args(t, TYPE_SLICE_BEGIN, thread_track(t), name, nargs, args);
}

void pf_instant_event_args_g_impl(const char *name, unsigned nargs, const struct pf_arg *args)
{
    struct pf_trace *t = default_trace();
    if (t)
        emit_named_args(t, TYPE_INSTANT, thread_track(t), name, nargs, args);
}

void pf_slice_begin_g_dyn_impl(const char *name)
{
    struct pf_trace *t = default_trace();
    if (t)
        emit_dyn(t, TYPE_SLICE_BEGIN, thread_track(t), name);
}

void pf_instant_event_g_dyn_impl(const char *name)
{
    struct pf_trace *t = default_trace();
    if (t)
        emit_dyn(t, TYPE_INSTANT, thread_track(t), name);
}

void pf_slice_begin_g_fmt_impl(const char *fmt, ...)
{
    char name[FMT_NAME_BYTES];
    va_list ap;
    va_start(ap, fmt);
    const char *s = format_name(name, sizeof(name), fmt, ap);
    va_end(ap);
    struct pf_trace *t = default_trace();
    if (t)
        emit_dyn(t, TYPE_SLICE_BEGIN, thread_track(t), s);
}

void pf_instant_event_g_fmt_impl(const char *fmt, ...)
{
    char name[FMT_NAME_BYTES];
    va_list ap;
    va_start(ap, fmt);
    const char *s = format_name(name, sizeof(name), fmt, ap);
    va_end(ap);
    struct pf_trace *t = default_trace();
    if (t)
        emit_dyn(t, TYPE_INSTANT, thread_track(t), s);
}

void pf_thread_name(const char *name)
{
    if (!name)
        return;
    copy_track_name(pf_tls.name, name);
    pf_tls.has_name = true;

    struct pf_trace *t = default_trace();
    if (!t)
        return;
    struct pf_tls_slot *s = &pf_tls.slots[t->index];
    if (s->trace == t && s->track_uuid)
        track_rename(t, s->track_uuid, pf_tls.name);
}

uint64_t pf_track_thread_uuid_g(void)
{
    struct pf_trace *t = default_trace();
    return t ? thread_track(t) : 0;
}

void pf_counter_set_int_g(uint64_t counter_track_uuid, int64_t value)
{
    struct pf_trace *t = default_trace();
    if (t)
        emit_counter(t, counter_track_uuid, (uint64_t)value, false);
}

void pf_counter_set_double_g(uint64_t counter_track_uuid, double value)
{
    struct pf_trace *t = default_trace();
    if (t) {
        uint64_t bits;
        memcpy(&bits, &value, 8);
        emit_counter(t, counter_track_uuid, bits, true);
    }
}
