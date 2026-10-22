#ifndef PFTRACE_H
#define PFTRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#if defined(__GNUC__) || defined(__clang__)
#define PF_API __attribute__((visibility("default")))
#else
#define PF_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct pf_trace;

enum pf_unit {
    PF_UNIT_UNSPECIFIED = 0,
    PF_UNIT_TIME_NS = 1,
    PF_UNIT_COUNT = 2,
    PF_UNIT_SIZE_BYTES = 3,
};

/**
 * enum pf_mode - When buffered events reach the output file.
 * @PF_MODE_SNAPSHOT: Never write unless asked. The buffer overwrites its
 *      oldest events indefinitely; pf_trace_dump() captures the window
 *      leading up to the call. Use this to debug a perf drop the program
 *      detects itself.
 * @PF_MODE_CONTINUOUS: Write automatically. Once &pf_config.watermark_pct of the chunks are committed, a background
 *      thread drains them, so the
 *      output file accumulates every event rather than a window. Events
 *      are dropped (and counted in &pf_stats.dropped) only if producers
 *      outrun that thread.
 */
enum pf_mode {
    PF_MODE_SNAPSHOT = 0,
    PF_MODE_CONTINUOUS = 1,
};

/**
 * enum pf_format - What the output file contains.
 * @PF_FORMAT_PROTOBUF: Perfetto protobuf. Each event is encoded into its
 *      thread's chunk when it is emitted, so the drain only copies bytes. The
 *      file opens directly in the Perfetto UI.
 * @PF_FORMAT_RAW: Fixed-size records plus name and track sidecars,
 *      copied with little encoding work. Converted to protobuf offline by
 *      the reader described in docs/raw_format.md. Requires a new or empty
 *      output file, and a little-endian host.
 */
enum pf_format {
    PF_FORMAT_PROTOBUF = 0,
    PF_FORMAT_RAW = 1,
};

/**
 * struct pf_config - Trace parameters, fixed for the life of the trace.
 * @path: Directory the output goes in. Required. Checked at open, and held
 *      for the life of the trace, since snapshot dumps open their files in
 *      it.
 * @name: Continuous: the output file name within @path. Required, and fixed
 *      for the life of the trace. Opened with O_APPEND, so an existing file
 *      is extended rather than truncated. Snapshot: must be %NULL, since
 *      each pf_trace_dump() names its own file.
 * @buffer_bytes: Total in-RAM buffering. Divided into chunks of
 *      @chunk_slots records' worth of bytes each; the count is rounded down.
 *      Must hold at least two chunks. 0 selects 8 MiB. This is the only
 *      sizeable allocation the library makes, and it happens once, at open.
 * @chunk_slots: Size of a chunk, in records. A chunk holds this many
 *      fixed-size records in raw format, and this many times 48 bytes of
 *      encoded events in protobuf, where an event takes fewer bytes, so more
 *      events fit. A thread fills one chunk at a time and hands it over when
 *      it is full, so this sets how often a thread takes shared state. 0 is
 *      rejected at open. pf_config_init() selects 256.
 * @max_threads: Threads that may be bound to this trace at once. A thread
 *      past the limit drops its events and counts them. A thread's slot is
 *      freed when it exits, so later threads can bind it. 0 is rejected at
 *      open. pf_config_init() selects 256.
 * @mode: Snapshot or continuous; see &enum pf_mode.
 * @format: Protobuf or raw output; see &enum pf_format. Fixed for the
 *      life of the trace, so a file never mixes the two.
 * @watermark_pct: Percentage of chunks that must be committed before the
 *      drain thread is woken, 1-99. 0 selects 75. Ignored unless @mode is
 *      %PF_MODE_CONTINUOUS.
 * @drain_interval_ms: Upper bound on how long a committed chunk waits
 *      before the drain thread writes it. 0 selects 100.
 *      Ignored unless @mode is %PF_MODE_CONTINUOUS.
 * @max_tracks: Capacity of the track table. 0 selects 256. The process
 *      track pf_trace_open() registers takes one entry. Tracks are
 *      re-emitted at the head of every dump, so they cost file space
 *      rather than buffer space.
 * @process_name: Name of the process track. %NULL uses the program's comm
 *      name, if the OS gives one. Copied, so it may be a computed string.
 * @clock_id: Clock every event timestamp is read from: %CLOCK_BOOTTIME,
 *      %CLOCK_MONOTONIC or %CLOCK_REALTIME. pf_config_init() sets
 *      %CLOCK_BOOTTIME. Note that %CLOCK_REALTIME is 0, so a config that
 *      was never passed to pf_config_init() selects it.
 */
struct pf_config {
    const char *path;
    const char *name;
    size_t buffer_bytes;
    size_t chunk_slots;
    unsigned max_threads;
    enum pf_mode mode;
    enum pf_format format;
    unsigned watermark_pct;
    unsigned drain_interval_ms;
    unsigned max_tracks;
    const char *process_name;
    clockid_t clock_id;
};

/**
 * struct pf_stats - Cumulative counters, for checking a trace is honest.
 * @events: Records committed to chunks. Excludes the tail still in each thread's chunk.
 * @chunk_bytes: Size of one chunk in bytes, after padding to a cache line.
 * @chunk_count: Chunks in the pool, fixed at open.
 * @pool_bytes: Bytes allocated for the chunk pool, %chunk_bytes times
 *      %chunk_count. This is the trace's main memory cost.
 * @meta_bytes: Bytes allocated for the writer table and the track table,
 *      both sized at open. The staging buffers are not included: they grow
 *      with use.
 * @chunk_slots: Event records per chunk, from &pf_config.chunk_slots.
 * @slot_bytes: Size of one raw event record in bytes. A protobuf event is
 *      variable length, so its size is not fixed; a chunk holds %chunk_slots
 *      times this many bytes of them, plus its header and padding.
 * @bytes_written: Bytes handed to write() across all dumps.
 * @dropped: Events discarded rather than recorded, which means the output
 *      file has a gap. Never counts the events %PF_MODE_SNAPSHOT
 *      intentionally overwrites -- those are that mode working as
 *      intended. In the default build this only happens in
 *      %PF_MODE_CONTINUOUS, when every chunk is committed or in use and the drain
 *      thread has not yet written them: either the buffer or the drain
 *      interval is too small. A thread past &pf_config.max_threads drops
 *      every event it emits.
 */
struct pf_stats {
    uint64_t events;
    uint64_t bytes_written;
    uint64_t dropped;
    uint64_t chunk_bytes;
    uint64_t chunk_count;
    uint64_t pool_bytes;
    uint64_t meta_bytes;
    uint64_t chunk_slots;
    uint64_t slot_bytes;
};

/**
 * pf_config_init() - Fill a config with defaults.
 * @cfg: Config to initialize.
 *
 * Sets every field to its default, leaving &pf_config.path and
 * &pf_config.name %NULL for the caller to set. Call this before changing
 * individual fields so later additions to the struct keep their defaults.
 */
PF_API void pf_config_init(struct pf_config *cfg);

/**
 * pf_trace_open() - Allocate the chunk pool and open the output file.
 * @cfg: Trace parameters. Copied, so it need not outlive this call. The
 *      strings it points to, &pf_config.path and &pf_config.name, are read
 *      here, except that the directory is held open for the life of the
 *      trace.
 *
 * Performs every allocation the trace will ever need, and in
 * %PF_MODE_CONTINUOUS starts the drain thread. After this returns, the
 * event functions neither allocate nor block on I/O.
 *
 * Return: A new trace context, or %NULL on failure, with errno set.
 * %EINVAL means @cfg or its path is %NULL, or @watermark_pct exceeds 99,
 * or the name does not fit the mode (missing or not a single path
 * component in %PF_MODE_CONTINUOUS, given in %PF_MODE_SNAPSHOT), or
 * @format is %PF_FORMAT_RAW on a big-endian host. %EEXIST means
 * @format is %PF_FORMAT_RAW in %PF_MODE_CONTINUOUS and the file already
 * has contents. Errors from opening the directory or file pass through.
 */
PF_API struct pf_trace *pf_trace_open(const struct pf_config *cfg);

/**
 * pf_trace_dump() - Write everything currently buffered.
 * @t: Trace context.
 * @name: In %PF_MODE_SNAPSHOT, the stem of the new file, required. The file
 *      is named &lt;name&gt;-&lt;UTC time&gt;-&lt;seq&gt;.pftrace, for example
 *      frame_stall-20261018T143007.512Z-0003.pftrace, in the directory given
 *      at open. An existing file of that exact name is overwritten. In
 *      %PF_MODE_CONTINUOUS, must be %NULL: the dump drains into the one file
 *      opened at open.
 * @file: Receives the name of the file the dump wrote, without the directory,
 *      NUL-terminated. Join it to the directory given at open to get the
 *      path. May be %NULL. In %PF_MODE_CONTINUOUS it is the name of the open
 *      file. Set only when the dump succeeds.
 * @size: Size of @file in bytes.
 *
 * Writes the committed chunks to the output file, preceded by the track
 * table so the result stands alone as a trace. In %PF_MODE_SNAPSHOT this
 * is the only thing that produces output -- call it when the program
 * notices the problem worth debugging.
 *
 * Safe to call from any thread, and safe to call concurrently with the
 * drain thread.
 *
 * Return: 0 on success, or -1 with errno set from the open or write.
 * %EINVAL means @name does not match the mode, or is not a single path
 * component. %ENAMETOOLONG means @file is too small for the name; nothing
 * is written in that case.
 */
PF_API int pf_trace_dump(struct pf_trace *t, const char *name, char *file, size_t size);

/**
 * pf_trace_flush() - Dump, then force the result to disk.
 * @t: Trace context.
 * @name: As for pf_trace_dump().
 * @file: As for pf_trace_dump().
 * @size: As for pf_trace_dump().
 *
 * pf_trace_dump() plus fdatasync(), on the file that dump wrote. Use
 * before a crash is likely; a plain dump is enough for the file to be
 * readable by other processes.
 *
 * Return: 0 on success, or -1 with errno set.
 */
PF_API int pf_trace_flush(struct pf_trace *t, const char *name, char *file, size_t size);

/**
 * pf_trace_stats() - Read the cumulative counters.
 * @t: Trace context.
 * @out: Filled with a snapshot of the counters.
 *
 * Worth checking after a run in %PF_MODE_CONTINUOUS: a nonzero
 * &pf_stats.dropped means the trace is missing events.
 */
PF_API void pf_trace_stats(const struct pf_trace *t, struct pf_stats *out);

/**
 * pf_trace_close() - Drop the caller's reference to a trace.
 * @t: Trace context to close, or %NULL.
 *
 * The trace is finished by its last reference. A thread that has emitted into
 * @t holds one until it exits, and the calling thread's own is given back here.
 * When the last one goes, the work is done: in %PF_MODE_CONTINUOUS the drain
 * thread is stopped and joined and any buffered events are written, so the
 * file holds every event; in %PF_MODE_SNAPSHOT nothing is written, so events
 * still in the buffer are lost unless pf_trace_dump() was called first. Then
 * the file is closed and the buffers are freed.
 *
 * If other threads are still bound, this returns at once, and the file is
 * complete only when the last of them exits. Such a thread may keep emitting
 * until then. The caller must not use @t after this call, and must not call
 * this concurrently with any other function on @t.
 */
PF_API void pf_trace_close(struct pf_trace *t);

/**
 * pf_track_thread() - Register a thread track.
 * @t: Trace context.
 * @pid: Process ID owning the thread.
 * @tid: Thread ID to record.
 * @name: Thread name, or %NULL.
 *
 * The track is a child of the trace's process track, registered by
 * pf_trace_open().
 *
 * Safe to call from any thread, concurrently with other pf_track_*() calls.
 *
 * Return: The new track's track_uuid, or 0 if the table is full.
 */
PF_API uint64_t pf_track_thread(struct pf_trace *t, int32_t pid, int32_t tid, const char *name);

/**
 * pf_track_counter() - Get or register a counter track.
 * @t: Trace context.
 * @parent_uuid: Parent track. 0 selects the trace's process track. Otherwise
 *      a thread track's uuid, as returned by pf_track_thread() or
 *      pf_track_thread_uuid_g().
 * @name: Counter track name, or %NULL. Names longer than the track name
 *      cap are truncated, and the truncated form is what is compared.
 * @unit: Unit the counter's values are measured in.
 *
 * Identity is @parent_uuid and @name: a counter track with the same parent
 * and name as an existing one is that track, and its uuid is returned. Two
 * objects that should be separate series must therefore use distinct
 * names, e.g. by including an id. Creating a track with an existing
 * identity does not add a table entry.
 *
 * Asking for an existing identity with a different @unit returns 0.
 *
 * Safe to call from any thread, concurrently with other pf_track_*() calls.
 *
 * Return: The track's track_uuid, or 0 if the table is full, @parent_uuid
 *      is not a thread track, or the unit conflicts with an existing track.
 */
PF_API uint64_t pf_track_counter(struct pf_trace *t, uint64_t parent_uuid, const char *name,
                                 enum pf_unit unit);

/**
 * PF_NAME_LITERAL() - Assert at compile time that @s is a string literal.
 * @s: The candidate name.
 *
 * Event names are stored in the chunks as a borrowed pointer and read much
 * later, on the drain thread, so they must outlive the trace. A string
 * literal always does. This rejects anything else where the mistake is
 * cheap to fix -- at the call site, during compilation -- rather than
 * leaving it to surface as garbled output or a crash in a dump.
 *
 * `"" s ""` is only a valid expression if @s is a string literal; for a
 * pointer or an array it is a syntax error. The sizeof test additionally
 * rejects an empty name, which would produce an unnamed slice.
 *
 * Deliberately not applied to pf_track_*(): those copy the name into the
 * track table at registration, so their storage is already owned and a
 * computed name (snprintf into a buffer) is perfectly safe there.
 */
#define PF_NAME_LITERAL(s) ((void)sizeof(char[sizeof("" s "") > 1 ? 1 : -1]), ("" s ""))

/**
 * pf_slice_begin() - Begin a slice on a track.
 * @t: Trace context.
 * @track_uuid: Track to begin the slice on.
 * @name: Slice name. Must be a string literal -- see PF_NAME_LITERAL().
 *
 * Writes into this thread's chunk and returns; no allocation and no I/O.
 * Takes no lock on the common path. The thread takes the pool lock once
 * per chunk, to exchange a full chunk for an empty one. The name itself is not
 * copied, only its address, which is why it has to be a literal.
 *
 * Slices on a given track must be ended in stack (LIFO) order via
 * pf_slice_end().
 */
#define pf_slice_begin(t, track_uuid, name) \
    pf_slice_begin_impl((t), (track_uuid), PF_NAME_LITERAL(name))

/**
 * pf_slice_begin_impl() - pf_slice_begin() without the literal check.
 * @t: Trace context.
 * @track_uuid: Track to begin the slice on.
 * @name: Slice name, or %NULL. Borrowed, not copied: it must remain valid
 *      and unchanged until the trace is closed.
 *
 * Call pf_slice_begin() instead. This is the real function behind that
 * macro, exposed for the rare caller whose name is genuinely not a
 * literal but is still immortal -- a pointer into a string table, say.
 * The lifetime requirement is then yours to honour, and violating it is
 * undiagnosed: the drain thread may read the name long after the call.
 */
PF_API void pf_slice_begin_impl(struct pf_trace *t, uint64_t track_uuid, const char *name);

/**
 * pf_slice_end() - End the most recently begun slice on a track.
 * @t: Trace context.
 * @track_uuid: Track whose most recently begun, not-yet-ended slice
 *      should be ended.
 */
PF_API void pf_slice_end(struct pf_trace *t, uint64_t track_uuid);

/**
 * pf_instant_event() - Record a single-point-in-time marker.
 * @t: Trace context.
 * @track_uuid: Track to record the marker on.
 * @name: Marker name. Must be a string literal -- see PF_NAME_LITERAL().
 *
 * Emits Perfetto's TYPE_INSTANT event -- there is no matching _end call.
 */
#define pf_instant_event(t, track_uuid, name) \
    pf_instant_event_impl((t), (track_uuid), PF_NAME_LITERAL(name))

/**
 * pf_instant_event_impl() - pf_instant_event() without the literal check.
 * @t: Trace context.
 * @track_uuid: Track to record the marker on.
 * @name: Marker name, or %NULL. Borrowed, not copied; see
 *      pf_slice_begin_impl() for the lifetime requirement.
 */
PF_API void pf_instant_event_impl(struct pf_trace *t, uint64_t track_uuid, const char *name);

/* --- Event arguments ----------------------------------------------------
 * A slice or instant may carry up to PF_MAX_ARGS numeric arguments. The
 * @name is then a printf-style format with one placeholder per argument:
 * %d %i (signed), %u %x %X (unsigned), %f %g %e (double). Only these bare
 * conversions are accepted -- no width, precision or %s -- so the format
 * can be checked against the arguments without parsing C.
 *
 * Each argument is an int or a double, chosen at compile time from its
 * type. Anything else (a pointer, a string) fails to compile.
 *
 * Every record pays for PF_MAX_ARGS argument words in its chunk, whether or not
 * it has arguments. Raising PF_MAX_ARGS costs buffer space for all events.
 */
#define PF_MAX_ARGS 2

/**
 * pf_slice_begin_args() - pf_slice_begin() with 1 to PF_MAX_ARGS arguments.
 * @t: Trace context.
 * @track_uuid: Track to begin the slice on.
 * @name: Format string, a literal -- see PF_NAME_LITERAL().
 * @...: One int or double per %-placeholder in @name. Each may be
 *      evaluated more than once, so it must have no side effects.
 *
 * Passing more arguments than PF_MAX_ARGS does not compile. Passing none
 * does not either: use pf_slice_begin() for a slice with no arguments.
 */
#define pf_slice_begin_args(t, track_uuid, name, ...)                                         \
    PF_PACK_CAT(PF_PACK_, PF_NARGS(__VA_ARGS__))(pf_slice_begin_args_impl, (t), (track_uuid), \
                                                 PF_NAME_LITERAL(name), __VA_ARGS__)

/**
 * pf_instant_event_args() - pf_instant_event() with 1 to PF_MAX_ARGS
 * arguments. See pf_slice_begin_args().
 */
#define pf_instant_event_args(t, track_uuid, name, ...)                                         \
    PF_PACK_CAT(PF_PACK_, PF_NARGS(__VA_ARGS__))(pf_instant_event_args_impl, (t), (track_uuid), \
                                                 PF_NAME_LITERAL(name), __VA_ARGS__)

/**
 * pf_slice_begin_args_impl() - pf_slice_begin_args() without the macro.
 * @nargs: Number of arguments, 1 to PF_MAX_ARGS.
 * @a0, @a1: Argument bits: an int64 as two's complement, or a double's
 *      IEEE-754 bits. Only the first @nargs are used.
 * @a0_dbl, @a1_dbl: True when the matching argument is a double.
 *
 * Call the macro instead; these are its expansion targets.
 */
PF_API void pf_slice_begin_args_impl(struct pf_trace *t, uint64_t track_uuid, const char *name,
                                     unsigned nargs, uint64_t a0, bool a0_dbl, uint64_t a1,
                                     bool a1_dbl);

/** pf_instant_event_args_impl() - pf_instant_event_args() without the macro. */
PF_API void pf_instant_event_args_impl(struct pf_trace *t, uint64_t track_uuid, const char *name,
                                       unsigned nargs, uint64_t a0, bool a0_dbl, uint64_t a1,
                                       bool a1_dbl);

/* Counts a macro's arguments, up to four. Used only to pick PF_PACK_n. */
#define PF_NARGS(...) PF_NARGS_(__VA_ARGS__, 4, 3, 2, 1, 0)
#define PF_NARGS_(_1, _2, _3, _4, N, ...) N
#define PF_PACK_CAT(a, b) PF_PACK_CAT_(a, b)
#define PF_PACK_CAT_(a, b) a##b

/* Converts one argument to its 64-bit bits. The default arm is the int
 * path, so a pointer or string lands there and the implicit conversion
 * is rejected by the compiler. */
static inline uint64_t pf_arg_bits_int_(int64_t v)
{
    return (uint64_t)v;
}

static inline uint64_t pf_arg_bits_double_(double v)
{
    uint64_t bits;
    __builtin_memcpy(&bits, &v, sizeof bits);
    return bits;
}

#define PF_ARG_BITS(v)               \
    _Generic((v),                    \
        float: pf_arg_bits_double_,  \
        double: pf_arg_bits_double_, \
        default: pf_arg_bits_int_)(v)
#define PF_ARG_IS_DOUBLE(v) _Generic((v), float: 1, double: 1, default: 0)

/* Expansion targets for the argument counts. Each takes the impl to call,
 * then the same arguments the impl takes, minus the packed ones. */
#define PF_PACK_1(fn, t, uuid, name, a) \
    fn((t), (uuid), (name), 1, PF_ARG_BITS(a), PF_ARG_IS_DOUBLE(a), 0, 0)
#define PF_PACK_2(fn, t, uuid, name, a, b)                                          \
    fn((t), (uuid), (name), 2, PF_ARG_BITS(a), PF_ARG_IS_DOUBLE(a), PF_ARG_BITS(b), \
       PF_ARG_IS_DOUBLE(b))
/* More than PF_MAX_ARGS: this name is never defined, so the call fails
 * to compile at the call site. */
#define PF_PACK_3(...) pf_too_many_args_for_PF_MAX_ARGS(__VA_ARGS__)
#define PF_PACK_4(...) pf_too_many_args_for_PF_MAX_ARGS(__VA_ARGS__)

/* --- Event arguments on the calling thread's track ---------------------
 * The _g forms of the argument macros above. Same rules: the format is a
 * literal, one argument per placeholder, up to PF_MAX_ARGS. */
#define PF_PACKG_1(fn, name, a) fn((name), 1, PF_ARG_BITS(a), PF_ARG_IS_DOUBLE(a), 0, 0)
#define PF_PACKG_2(fn, name, a, b) \
    fn((name), 2, PF_ARG_BITS(a), PF_ARG_IS_DOUBLE(a), PF_ARG_BITS(b), PF_ARG_IS_DOUBLE(b))
#define PF_PACKG_3(...) pf_too_many_args_for_PF_MAX_ARGS(__VA_ARGS__)
#define PF_PACKG_4(...) pf_too_many_args_for_PF_MAX_ARGS(__VA_ARGS__)

/**
 * pf_slice_begin_args_g() - pf_slice_begin_g() with 1 to PF_MAX_ARGS
 * arguments. See pf_slice_begin_args().
 */
#define pf_slice_begin_args_g(name, ...)                                      \
    PF_PACK_CAT(PF_PACKG_, PF_NARGS(__VA_ARGS__))(pf_slice_begin_args_g_impl, \
                                                  PF_NAME_LITERAL(name), __VA_ARGS__)

/**
 * pf_instant_event_args_g() - pf_instant_event_g() with 1 to PF_MAX_ARGS
 * arguments. See pf_slice_begin_args().
 */
#define pf_instant_event_args_g(name, ...)                                      \
    PF_PACK_CAT(PF_PACKG_, PF_NARGS(__VA_ARGS__))(pf_instant_event_args_g_impl, \
                                                  PF_NAME_LITERAL(name), __VA_ARGS__)

/**
 * pf_slice_begin_args_g_impl() - pf_slice_begin_args_g() without the macro.
 * Takes the same argument encoding as pf_slice_begin_args_impl().
 */
PF_API void pf_slice_begin_args_g_impl(const char *name, unsigned nargs, uint64_t a0, bool a0_dbl,
                                       uint64_t a1, bool a1_dbl);

/** pf_instant_event_args_g_impl() - pf_instant_event_args_g() without the macro. */
PF_API void pf_instant_event_args_g_impl(const char *name, unsigned nargs, uint64_t a0, bool a0_dbl,
                                         uint64_t a1, bool a1_dbl);

/**
 * pf_counter_set_int() - Sample a counter track with an integer value.
 * @t: Trace context.
 * @counter_track_uuid: Counter track to sample.
 * @value: Value to record.
 */
PF_API void pf_counter_set_int(struct pf_trace *t, uint64_t counter_track_uuid, int64_t value);

/**
 * pf_counter_set_double() - Sample a counter track with a floating-point
 *      value.
 * @t: Trace context.
 * @counter_track_uuid: Counter track to sample.
 * @value: Value to record.
 */
PF_API void pf_counter_set_double(struct pf_trace *t, uint64_t counter_track_uuid, double value);

/* --- Simple API ---------------------------------------------------------
 *
 * The calls below take no trace and no track. They act on the calling
 * thread's own track, on the process default trace:
 *
 *   - The default is the trace pf_trace_open_g() opened. It is meant to be
 *     opened once, by the main thread, and to live for the whole process.
 *     pf_trace_close_g() clears it. pf_trace_open() and pf_trace_close()
 *     leave it alone.
 *   - A thread's track is created on its first _g call. The library calls
 *     gettid() and getpid() at that point and registers a thread track with
 *     them. The track is a child of the process track, which pf_trace_open_g()
 *     registers for getpid().
 *   - The uuid is cached in thread-local storage. Once cached, a call costs
 *     a pointer load, a thread-local read and a flag check. No syscall.
 *   - Track names: pf_thread_name() sets it. If the track does not exist
 *     yet, the name applies at creation. If it does, the track is renamed
 *     and its descriptor re-emitted under the same uuid. Without a call,
 *     the name is the OS thread name, or "thread-<tid>".
 *
 * Use the explicit API (pf_track_thread() and the calls that take a track)
 * when a thread needs to write to a trace other than the default, or when
 * the track must be created up front.
 */

/**
 * pf_slice_begin_g() - Begin a slice on the calling thread's track.
 * @name: Slice name. Must be a string literal -- see PF_NAME_LITERAL().
 */
#define pf_slice_begin_g(name) pf_slice_begin_g_impl(PF_NAME_LITERAL(name))

/**
 * pf_slice_begin_g_impl() - pf_slice_begin_g() without the literal check.
 * @name: Slice name, or %NULL. Borrowed, not copied; see
 *      pf_slice_begin_impl() for the lifetime requirement.
 */
PF_API void pf_slice_begin_g_impl(const char *name);

/**
 * pf_slice_end_g() - End the calling thread's most recently begun slice.
 */
PF_API void pf_slice_end_g(void);

/**
 * pf_instant_event_g() - Record a marker on the calling thread's track.
 * @name: Marker name. Must be a string literal -- see PF_NAME_LITERAL().
 */
#define pf_instant_event_g(name) pf_instant_event_g_impl(PF_NAME_LITERAL(name))

/**
 * pf_instant_event_g_impl() - pf_instant_event_g() without the literal
 *      check.
 * @name: Marker name, or %NULL. Borrowed, not copied.
 */
PF_API void pf_instant_event_g_impl(const char *name);

/**
 * pf_thread_name() - Name the calling thread's track.
 * @name: Thread name. Copied, so it may be a computed string.
 *
 * May be called before the thread's first _g call, or at any time after.
 * A rename after registration is picked up by the next flush.
 */
PF_API void pf_thread_name(const char *name);

/**
 * pf_track_thread_uuid_g() - Get the calling thread's track uuid from TLS.
 *
 * Reads the uuid cached in this thread's TLS slot on the default trace. The
 * track is registered on the first call, as with the other _g calls. Use it
 * as the @parent_uuid of pf_track_counter() to nest a counter under the
 * calling thread.
 *
 * Return: The thread track's track_uuid, or 0 if there is no default trace.
 */
PF_API uint64_t pf_track_thread_uuid_g(void);

/**
 * pf_track_counter_g() - pf_track_counter() on the default trace.
 * @parent_uuid: Parent track, as for pf_track_counter(). 0 selects the
 *      process track.
 * @name: Counter track name, or %NULL.
 * @unit: Unit the counter's values are measured in.
 *
 * Return: The track's track_uuid, or 0 if there is no default trace or the
 *      call would fail for the reasons pf_track_counter() gives.
 */
PF_API uint64_t pf_track_counter_g(uint64_t parent_uuid, const char *name, enum pf_unit unit);

/**
 * pf_counter_set_int_g() - pf_counter_set_int() on the default trace.
 * @counter_track_uuid: Counter track to sample.
 * @value: Value to record.
 */
PF_API void pf_counter_set_int_g(uint64_t counter_track_uuid, int64_t value);

/**
 * pf_counter_set_double_g() - pf_counter_set_double() on the default trace.
 * @counter_track_uuid: Counter track to sample.
 * @value: Value to record.
 */
PF_API void pf_counter_set_double_g(uint64_t counter_track_uuid, double value);

/**
 * pf_trace_dump_g() - pf_trace_dump() on the default trace.
 * @name: As for pf_trace_dump().
 * @file: As for pf_trace_dump().
 * @size: As for pf_trace_dump().
 *
 * Return: As for pf_trace_dump(), or -1 with %ENODEV if there is no default
 *      trace. Unlike the other _g calls this does not succeed silently
 *      without one, because the caller expects a file.
 */
PF_API int pf_trace_dump_g(const char *name, char *file, size_t size);

/**
 * pf_trace_flush_g() - pf_trace_flush() on the default trace.
 * @name: As for pf_trace_flush().
 * @file: As for pf_trace_flush().
 * @size: As for pf_trace_flush().
 *
 * Return: As for pf_trace_flush(), or -1 with %ENODEV if there is no default
 *      trace.
 */
PF_API int pf_trace_flush_g(const char *name, char *file, size_t size);

/**
 * pf_trace_open_g() - pf_trace_open(), and make the result the default trace.
 * @cfg: As for pf_trace_open().
 *
 * On success the simple API calls write to the new trace until
 * pf_trace_close_g() is called. The trace itself is not returned.
 *
 * Return: 0 on success, or -1 with errno set as for pf_trace_open(). On
 *      failure the default is left unset.
 */
PF_API int pf_trace_open_g(const struct pf_config *cfg);

/**
 * pf_trace_close_g() - pf_trace_close() on the default trace, and clear it.
 *
 * Does nothing if there is no default trace. Use this rather than
 * pf_trace_close() on the default: that leaves the default pointing at a
 * freed trace.
 */
PF_API void pf_trace_close_g(void);

/**
 * pf_trace_default_g() - Get the default trace, for the explicit API.
 *
 * Return: The trace pf_trace_open_g() opened, or %NULL if there is none.
 *      Valid until pf_trace_close_g(). Use it with the calls that take a
 *      trace, such as pf_track_thread() or pf_trace_dump(), and never pass
 *      it to pf_trace_close(): use pf_trace_close_g().
 */
PF_API struct pf_trace *pf_trace_default_g(void);

#ifdef __cplusplus
}
#endif

#endif /* PFTRACE_H */
