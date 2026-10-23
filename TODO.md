# TODO

Performance and behaviour work on the chunk buffers. See
`docs/chunk_buffers_design.md` for the design and its known limits.

## 1. Dynamic chunk count per thread

Give high-rate threads more chunks and low-rate threads fewer, instead of a
fixed number per thread. A thread's chunk turnover (how often it fills a chunk)
shows its rate, so the pool can be rebalanced from that, or the user can set a
share per thread.

Trade-off: the pool needs per-thread accounting and a way to move chunks
between threads. Rebalancing is only done at chunk rotation, so the hot path
does not change. A quiet thread that suddenly bursts must wait for chunks to be
returned. A thread that goes idle holds its chunks until they are reclaimed.

## ~~2. Tails of live idle threads in dumps~~ (done)

~~Done. Snapshot dumps walk every writer's partial chunk up to the count the
writer has published, so a live idle thread's records are in the next dump. The
cost is one release store per event, measured with `bench_chunk` within noise.~~

~~Done in continuous mode and at close. The trace is held by a reference count:
the owner and each bound thread hold one. Close drops the owner's, and the last
reference to go finishes the trace. A thread's partial chunk is committed when
it unbinds, so it reaches the file at its exit. The file is complete at close if
no thread is bound, and otherwise at the last bound thread's exit. Close does
not commit another thread's chunk, so the race with a producer is gone.~~

## ~~3. Encode on the producer~~ (done)

~~Done for protobuf. Steps 1 to 5 are complete, and the drain's per-event cost
fell from about 70 ns to 1 to 3 ns. The producer cost rose, which is the open
trade-off recorded below. The remaining question, how format strings are
handled for args events, is item 9.~~

Move protobuf encoding onto the producer, as Perfetto's SDK does: each record is
encoded into its chunk on the emitting thread, and the drain only copies chunk
bytes out. The drain's per-event work (`bench`: about 100 ns/event) then leaves
the drain thread, which is what limits the continuous-mode rate today.

Trade-off: the hot path gets more expensive, since encoding runs inside the
caller. Chunks become byte buffers with variable record sizes, so the
fixed-slot layout and `chunk_slots` as a record count no longer hold. Encoding
needs interned names and track ids available on the producer. Measure with
`bench_chunk` before and after.

Decided: a record that does not fit the rest of its chunk moves to the next
chunk. It is not split across two chunks. Perfetto's SDK does split, with
continuation flags in the chunk header (`kLastPacketContinuesOnNextChunk`,
`kFirstPacketContinuesFromPrevChunk`) and a reader that joins the pieces by
writer id and chunk id. Splitting would need the drain to hold partial records
and join them, and to patch nested size fields (`kChunkNeedsPatching`). Not
splitting keeps every chunk complete on its own, so the drain only copies bytes.
The cost is the unused tail of each chunk.

Interned names are per writer, as in Perfetto. The dictionary lives in the
writer's incremental state (`TrackEventIncrementalState`, packet-sequence
scoped), and is reset with `SEQ_INCREMENTAL_STATE_CLEARED`. Each thread interns
its own names, so the producer needs no shared table on the hot path.

Proposal:

1. Chunk layout. Chunks become byte buffers. The header records used bytes
   instead of a record count, and `chunk_slots` becomes a byte capacity or is
   dropped.
2. Producer encoding. Each event is encoded into the thread's current chunk. If
   the record does not fit, the chunk is committed and the record starts a new
   one.
3. Producer-side intern cache. Each writer keeps its own name and track id map.
   A first use emits the definition into the same chunk, ahead of the record
   that uses it.
4. Drain. Copies committed chunk bytes into the output. It no longer calls the
   encoder.
5. Snapshot dumps and close. The partial-chunk walk (item 2) reads a published
   byte offset instead of a record count.
6. Raw format stays on the current path. Producer encoding applies to protobuf
   only.

Expected: chunk storage falls from 48 to about 25 bytes per event on the simple
workload, about 1.9 times as many events per pool, so drops at 8 and 12 threads
should fall. The drain's per-event cost should drop well below 70 ns. The
producer cost rises by the encoding time per event, and that rise is the main
open question.

Steps:

1. Prototype: encode a fixed record on the producer and measure with
   `bench_chunk` and `bench_side`. This checks whether the producer can absorb
   the encoding cost.
2. Implement the byte-chunk layout, the intern cache, and the drain copy.
3. Update snapshot dump and close to use byte offsets.
4. Measure both workloads at 1, 4, 8 threads against the baseline below. Success
   means a lower consumer tail, and fewer or no drops at 8 threads.
5. Run the full ctest suite and the sanitized builds.

Step 1 result, from `tests/bench_encode.c` on one thread, 20M events, 4096
records per chunk:

| encoder | ns/event | bytes/event |
|---|---:|---:|
| plain 48-byte store (today's producer) | 1.4 | 48.0 |
| encoded, copied through scratch buffers | 51.0 | 19.2 |
| encoded, written direct into the chunk | 3.8 | 27.2 |

Copying through scratch buffers costs too much: each submessage is built in a
buffer and then copied into its parent, and the packet is copied into the chunk.
Writing direct avoids the copies. Each length is reserved as a 5-byte padded
varint and filled in when the body is written, which is valid protobuf. The
padding is why it takes 27.2 bytes against 19.2 minimal, and chunk storage still
falls from 48 to 27 bytes per event.

The 3.8 ns is a floor. The real encoder adds a fits-in-chunk check per record
and a per-writer intern lookup on each event, and the realistic mix adds argument
instants. None of those are in the bench yet.

Baseline, before the change, from `tests/bench_side.c` in continuous mode with
the drain live. Each thread emits 2M begin/end pairs (4M events), `chunk_slots`
4096, 2 GiB buffer, default watermark and drain interval. Single runs.

Hardware: Intel Core i7-12700 (12th gen), 12 cores with 2 threads each, 20
logical CPUs, one socket, one NUMA node. Max 4.9 GHz, min 800 MHz. Caches: 48
KiB L1d per core, 1.25 MiB L2 per core, 25 MiB L3 shared. 31 GiB RAM. Linux
5.15.0-198-generic.

| threads | producer ns/event | consumer tail ns/event | dropped |
|---:|---:|---:|---:|
| 1 | 7.0 | 73.6 | 0 |
| 4 | 7.9 | 69.9 | 0 |
| 8 | 14.3 | 69.6 | 0 |

The producer column is the caller's cost per event. The consumer tail is the
drain's backlog after the producers stop, divided by events. It is a lower
bound on the drain's per-event cost, since the drain also runs during the
producer phase. At 12 threads the drain falls behind even with a 2 GiB buffer,
dropping events at every chunk size tried (256 to 65536), so 12 threads is left
out of the baseline. The drain's cost, about 70 ns/event, is what this change
should move off the drain thread. The producer cost should rise, and the drop
count at 8 and 12 threads is the number to watch.

Second baseline, on a different machine: Intel Core i5-12500H, 8 logical CPUs,
one thread per core, 144 MiB L3, kernel 6.18.52. Built outside CMake with
`cc -O2 -g -fno-omit-frame-pointer` from a copy of `tests/bench_side.c` in
`/tmp`, one workload per binary. Five runs per workload, producer ns/event per
emitting thread:

| workload | threads | mean | min | max |
|---|---:|---:|---:|---:|
| simple | 1 | 17.0 | 16.8 | 17.3 |
| simple | 4 | 31.1 | 24.1 | 40.9 |
| simple | 8 | 44.6 | 43.7 | 47.3 |
| realistic | 1 | 26.9 | 25.7 | 27.9 |
| realistic | 4 | 55.4 | 46.3 | 64.4 |
| realistic | 8 | 71.0 | 63.3 | 76.0 |

Run-to-run spread is small at 1 thread and wide at 4 and 8, so compare before and
after with interleaved runs in one session.

Status: steps 1 to 5 are done for protobuf. Raw keeps the record walk, since it
is fixed-size. Snapshot and close use the same drain, which copies each chunk up
to its published byte count.

Result, after the change, same bench and hardware, single runs, 0 dropped in every
row:

| workload | threads | producer ns/event | consumer tail ns/event | baseline producer | baseline tail |
|---|---:|---:|---:|---:|---:|
| simple | 1 | 25.2 | 10.9 | 7.0 | 73.6 |
| simple | 4 | 44.9 | 2.7 | 7.9 | 69.9 |
| simple | 8 | 45.1 | 1.4 | 14.3 | 69.6 |
| realistic | 1 | 39.7 | 10.0 | 8.7 | 86.5 |
| realistic | 4 | 73.0 | 2.6 | 12.6 | 75.7 |
| realistic | 8 | 68.5 | 1.3 | 22.8 | 75.5 |

The realistic baseline is HEAD measured in the same session, before the byte
layout; the simple baseline is the table above.

What this shows:

- The drain's per-event cost fell from about 70 ns to 1 to 3 ns. That was the goal
  of the change, and it is met.
- The producer cost rose. At one thread on the simple workload it went from 7.0
  to 25.2 ns, about 3.6 times the baseline. This is the open trade-off. The
  encoder floor measured in step 1 was 3.8 ns, so the real encoder costs about 6
  times that floor.
- Profiling one thread with callgrind found the producer's time in the encoder.
  The first version recomputed each submessage's length by encoding it again,
  which cost about twice the instructions. Computing each length once, from the
  fields, brought the one-thread run from 145G to 78G instructions. The later
  change to reuse the lengths within an event gave about 20% on the one-thread
  simple run.
- The remaining producer cost is the encoder itself: byte-at-a-time writes, a
  32-slot name lookup per event, and per-event bookkeeping. Writing the fields
  straight into the chunk without a length pass is the next candidate, and is not
  tried yet.
- Drops at 12 threads were not re-measured. The drain now costs less, so this is
  the first place to look for a change.

Repeat with the name-length cache (`d7da722`), same bench and hardware. Two runs
of threads 1, 4, 8 and one run of 12 threads, each with 0 dropped. The producer
column is the mean of the two runs for 1, 4 and 8 threads; the spread between
runs was 10 to 20 percent.

| workload | threads | run 1 | run 2 | mean producer ns/event | consumer tail ns/event (run 1, run 2) |
|---|---:|---:|---:|---:|---:|
| simple | 1 | 24.9 | 24.8 | 24.9 | 15.4, 10.6 |
| simple | 4 | 39.5 | 47.7 | 43.6 | 4.1, 2.7 |
| simple | 8 | 49.3 | 43.7 | 46.5 | 2.2, 1.5 |
| realistic | 1 | 39.0 | 47.5 | 43.3 | 16.0, 10.0 |
| realistic | 4 | 57.9 | 69.2 | 63.6 | 4.0, 2.5 |
| realistic | 8 | 63.7 | 61.3 | 62.5 | 2.0, 1.2 |
| simple | 12 | 43.9 | | | 1.7 |
| realistic | 12 | 77.5 | | | 0.9 |

Removing the per-event `strlen` is within the run-to-run noise on these numbers.
The realistic 4 and 8 thread means are lower than the table above, but the
realistic 1 thread mean is higher, so no producer gain is claimed.

At 12 threads there are no drops in either workload, so the drop note above no
longer holds for the current code. It was one run each, with the 12-thread row
built as a temporary variant of `tests/bench_side.c` that runs only 12 threads.

Tried: padded 5-byte length prefixes, reserved while the record is written and
filled in at the end of its body, so the writer no longer computes nested
lengths or runs the varint loops. The size pass stays for the chunk fit check.
`bench_side` instructions fell from 57.8G to 56.9G, 1.4%, and all 46 ctest tests
passed. `bench_chunk`, RelWithDebInfo, two runs each, ns/event per thread:

| threads | HEAD | padded lengths |
|---:|---:|---:|
| 1 | 15.5, 16.5 | 16.5, 16.1 |
| 2 | 16.4, 15.9 | 16.9, 16.9 |
| 4 | 17.7, 17.6 | 18.8, 19.0 |
| 8 | 23.7, 26.8 | 28.6, 29.2 |

No gain at 1 or 2 threads, and 6 to 10 percent slower at 4 and 8. The padding
adds bytes per event, which may rotate chunks more often; not profiled. Reverted.
The encoder's length work is not where the time goes. `memcpy` is 6% of
instructions, and the encoder accounts for about 1% of that.

## ~~4. Chunk boundaries may share a cache line~~ (done)

~~Done. `chunk_bytes` is rounded up to a multiple of 64, and the pool is
allocated with `aligned_alloc(64, …)` and zeroed by hand, so no two chunks share
a line. `bench_chunk`, `chunk_slots` 256, ns/event, three runs each:

| threads | before | after |
|---:|---:|---:|
| 1 | 5.0, 6.6, 5.9 | 5.6, 5.6, 5.6 (and 5.1, 6.7, 6.0, 5.1 in a second set) |
| 8 | 11.4, 11.7, 11.6 | 8.8, 9.1, 8.8 |

The 8-thread gain is about 2.7 ns/event. The 1-thread runs are within noise.
All 46 ctest tests pass.~~

~~Trade-off: each chunk is padded to a line, so the pool holds fewer chunks in
the same `buffer_bytes`. At the 256-slot default the padding is small. For small
buffers it is not: a 4-slot chunk is 216 bytes and pads to 256, so a buffer sized
for two unpadded chunks holds only one, and `pf_pool_init` fails with `EINVAL`.
`test_chunk` was sized from the unpadded size and failed on this; its
`CHUNK_BYTES` now rounds up the same way.~~

## ~~5. Return a writer slot when its thread exits~~ (done)

~~Done. A thread's exit gives its writer slot back: after the partial chunk is
committed (or returned to the pool if it is empty), `bound` is cleared under
`reg_lock`. Bind already resets `cur`, `used` and `seq`, and the event count is
kept, so stats still count the slot's events. `test_slot_reuse` runs six
sequential threads against two slots: no drops, 60 events counted. It failed
before the change (20 counted, 40 dropped). `bench_chunk` is unchanged, since
only the thread-exit path changed.~~

~~A writer slot is not reclaimed when its thread exits, so a trace binds at most
`max_threads` threads over its life. Threads past that limit drop their events.
Option: once the exiting thread's partial chunk is committed, the slot goes back
to the unbound state so a later thread can take it. The slot's event count must
survive the reuse, or stats would lose the events already emitted. Only the
thread-exit destructor and bind touch `bound`, under `reg_lock`, so the hot path
does not change.~~

~~Trade-off: a slot is then reused by a different thread, so the writer's per-slot
state (`seq`, `cur`) has to be reset on bind. A thread's TLS cache is released
when it unbinds, so no cache can point at a slot after its thread has gone.
Test with a trace that outlives more threads than `max_threads`.~~

## ~~6. Replace the free list with a bitmap~~ (cancelled)

~~The free list threads its chunks through their `next` fields, under `pool_lock`.
Option: keep one bit per pool chunk, indexed from the chunk's offset in
`pool_mem`. Taking a chunk is then a scan for a set bit, and returning one is a
bit set. Nothing is written into the chunk headers to track free chunks, and
the pool's invariants are easier to check.~~

~~Trade-off: the scan is linear in the pool size. At the default 8 MiB with 256
slots that is about 680 chunks, or 11 words, so it is cheap, but a large
`buffer_bytes` with small chunks would make it longer. A bitmap does not help
the committed FIFO, which still needs order. The `next` field is also borrowed
by the snapshot partial-chunk walk, so that walk may need a separate list after
this change.~~

Cancelled after measuring. The bitmap (with a free-word hint, `take` as a
ctz scan, `release` as a bit set) passed all 58 ctest tests. `bench_chunk`,
1 thread, ns/event, 3 runs each:

| chunk_slots | free list | bitmap |
|---|---|---|
| 16 | ~7.0 | ~10.7 |
| 64 | ~6.2 | ~8.7 |
| 256 | ~5.3 | ~5.3–6.6 (noise) |

The regression is in rotation, which runs once per chunk. The release path
divides the chunk offset by `chunk_bytes` to find its bit, and that division is
the likely cause; not profiled. The free list's pop is already O(1), so the
bitmap gains nothing in speed. Reverted.

## ~~7. Let a thread bind to more than one trace (low priority)~~ (done)

~~Done. `pf_tls` has one slot per open trace, indexed by the trace's index, so a
thread holds a writer in every trace it emits to. Binding a second trace no
longer releases the first.~~

~~Trade-off: the cap is `PF_MAX_TRACES` (8) open traces, and opening a ninth fails
with `ENOSPC`. The lookup costs about the same as the single slot at 8 threads
(within noise). At 1 thread it may cost 0.5–1 ns per event, which is about the
size of the noise, so this is not settled. The simple API's thread track uuid is
still one TLS slot (`tl_slot` in `pftrace.c`), so the uuid is for one trace
only; that is left as it is.~~

## 8. Bounded, rotating output in continuous mode

Continuous mode writes one file that grows without limit. Make the output
bounded and rotating, with both limits off by default so the current single-file
behaviour stays the same:

1. Size limit: a config field sets a maximum file size. When the next write would
   exceed it, the drain closes the file and opens a new one.
2. File count limit: a config field sets the maximum number of files. Past that,
   the new file overwrites the oldest one.
3. Default off: both fields are 0, which gives today's single unbounded file.

Trade-off: the drain has to check the size before each write and close and open
files at the boundary, so the size check is on the drain path, not the producer
path. A record must not straddle two files, so a file can end short of the limit
and the boundary goes between drain batches. Overwriting the oldest file loses
events, so the dropped count (or a new counter) must say so, and the stats
should report the files written. Only `PF_MODE_CONTINUOUS` rotates; snapshot
dumps keep their own file naming.

## ~~9. Name formatting for args events~~ (done: caller formats)

Decided: the caller formats its own name and passes it in, as Perfetto does.
Percceto no longer expands `%` specs, and the `uint64_t arg[PF_MAX_ARGS]` words
are gone, so an event's record is a fixed head plus, for a dynamic name, a copy
of the name. `pf_format.c` and the `_args` API were removed.

- `pf_slice_begin` and `pf_instant_event` take a name built by the caller too: a literal goes static, anything else takes the dynamic path.
  Protobuf sends it inline and does not intern it. Raw copies it into the record
  (`RF_NAME_COPIED`, format version 5). Names over 128 bytes are cut to 125 bytes
  ending in `...`, and the cut is counted in `pf_stats.truncated`.
- A printf format with arguments is also accepted (`pf_slice_begin(t, trk, "req-%u", n)`).
  The macro formats into a buffer for the call, then takes the dynamic path.
- Static names are unchanged: protobuf interns them by pointer, raw keeps a
  pointer and defines it in the names chunk.

The trade-offs are in `docs/raw_format.md` ("Variable-size records") and
`docs/chunk_buffers_design.md`. The cost of formatting moves to the caller.
Typed debug annotations on the protobuf path are item 10.

Benchmark (`tests/bench_side.c`, Release build, continuous mode, 2M iterations
per thread, 0 dropped in every row). Producer ns per event; the simple workload
is one begin and end per iteration:

| variant | 1 thread | 4 threads | 8 threads |
|---|---:|---:|---:|
| literal, `pf_slice_begin(t, trk, "compute")` | 19.9 | 31.4 | 38.3 |
| formatted, `pf_slice_begin(t, trk, "req-%u", n)` | 52.1 | 49.2 | 54.9 |
| manual, `snprintf` then `pf_slice_begin_dyn_impl` | 50.4 | 55.2 | 59.9 |

The formatted macro costs the same as formatting by hand, and about 30 ns per
event more than a literal at one thread. These rows come from a 20-core machine,
not the i5-12500H in the table above, so compare within a table only.

## 10. Typed args on the protobuf path

Send event arguments as typed debug annotations instead of expanded text, as
Perfetto's SDK does. The name is the static string, interned like any other
literal, and each argument is a `DebugAnnotation` (`debug_annotation.proto`,
v58.3) with its own type: int, uint, or double. There is no string kind. A
string value is a format string's job, and the caller builds the name instead,
so `PF_KV` rejects pointers at compile time.

Trade-off: the slice bar shows the static name only. Values appear in the
slice's arguments section in the details panel, not on the bar, so debug text
is no longer visible at a glance. The proto header says debug annotations are
not a stable API, so readers must not depend on their field layout. The event
size now varies with the argument values, since each annotation is a
length-prefixed field. The producer does not format text, so the cost is the
annotation encoding, not `snprintf`. `PF_MAX_ARGS` is 4, overridable with
`-DPF_MAX_ARGS=n`; each call reserves a stack array of that many. Raw does not
carry args yet; see item 12.

Producer cost on events with no args is back to HEAD's: the regression
was a `memset` of `struct pb_event`, which grew with the argument fields and
was no longer cleared inline. The encoder now sets each field it reads, so the
struct is not zeroed as a whole.

`bench_side`, 1 thread, three runs each (ns/event):

| workload  | HEAD            | this change     |
|-----------|-----------------|-----------------|
| simple    | 20.1, 19.3, 19.4 | 17.6, 18.0, 17.9 |
| realistic | 32.1, 32.4, 32.2 | 32.2, 32.1, 32.3 |

Before the fix, the simple workload ran at about 24 ns/event.

Regression check on the current machine (i5-12500H, gcc 15.3, `bench_side`
Release, 1 thread, three interleaved runs each, producer ns/event): `146f39c`
realistic 28.5, `69c7460` realistic 30.5, simple 17.7 and 17.8. Instruction
counts from callgrind (100k events per workload) are within 0.5 percent of each
other: 424.9M for `146f39c`, 423.4M for `69c7460`, so the extra time is not
extra work. Passing the address of the local `pb_args` into the size and encode
helpers on the no-argument path cost about 2 ns on realistic. Passing a static
empty `pb_args` on that path recovered it, at 28.5 ns. The mechanism is not
confirmed; padding the stack frame alone did not recover it. `src/pf_encode.c`
is back at `146f39c` for readability.

Args workload: `bench_side` has an `args` workload, which calls
`pf_slice_begin_args` with two `PF_KV` arguments per begin and end pair. One
thread, producer ns/event, gcc 15.3, three runs: simple 16.5 to 17.4, args 26.6
to 27.0. Clang 21.1.8, one run: simple 17.8, realistic 26.9, args 25.4.

Clang regression check: realistic, one thread, producer ns/event, three runs.
`146f39c` gives 26.9, 27.2, 27.6, and `69c7460` gives 27.2, 27.0, 27.0. The
regression above does not show with clang, so it is specific to gcc.

Still open: none for item 10 except the docs, which now describe the args API
in both design docs.

Depends on item 9: the caller-formatted name path is removed first. Measure
with `bench_side`'s realistic workload before and after.

## 11. C++ scoped-slice macros take caller-built names

`PF_SCOPED_SLICE` and `PF_SCOPED_SLICE_G` in `include/pftrace.hpp` still use
`PF_NAME_LITERAL`, so they reject a name the caller built. The C macros
`pf_slice_begin`, `pf_instant_event` and their `_g` forms dispatch on constness
now, and the C++ ones should follow. Once they do, `PF_NAME_LITERAL` has no
callers and can go. Check the C++ wrapper's `pf::Slice` and `pf::ThreadSlice`
constructors against the dispatching macros, since they take `const char *`
and are the other place a static-buffer pointer can be kept.

## 12. Raw format accepts args

The typed args of item 10 are dropped on the raw path: raw v5 records have a
fixed 32-byte head and no field for them. Adding them means a v6 record with a
typed argument section. The changes that come with it:

- The record layout and its length rules, in `docs/raw_format.md`.
- The raw writer's args path in `src/pftrace.c`, which now ignores them.
- `tools/pfraw2pb.py`, which reads v5 and needs to emit the args as
  DebugAnnotations.
- The reader in `tests/test_raw_roundtrip.c`, written from the spec.

Decide first whether raw needs args at all. Protobuf is the format that
carries them today, so this is worth doing only if raw traces need the same
values.


- Continuous mode has no timer flush. A thread's partial chunk is written when
  it unbinds (the thread exits, or closes the trace), or when it fills. Events
  still in a live thread's partial chunk are lost only if the process dies first.
- Encoding stays on the drain thread.

## 13. Intern debug annotation names

`enc_annotation` in `src/pf_encode.c` writes each argument's key inline, on
every event. Keys are string literals, so they can be interned like event
names, and that would cut the bytes and encode work on the args path.

- Define each key once in the packet that first uses it, as an
  `InternedData.debug_annotation_names` entry (field 3, the same iid and name
  shape as `event_names`).
- Send `DebugAnnotation.name_iid` (field 1) in place of the inline name
  (field 10).
- Keep a per-writer key cache with the same epoch logic as the event-name
  cache, so a rotated chunk redefines its keys.

Also enforce that a key is a literal. `PF_KV` takes its key as a plain
argument today, so a caller could pass a buffer that is reused, and a cache
keyed on its pointer would then name the wrong key. Check the key the way
`PF_NAME_LITERAL` checks names.

This does not help the no-args regression in item 10; it only changes the
args path.

## Notes

- Continuous mode has no timer flush. A thread's partial chunk is written when
  it unbinds (the thread exits, or closes the trace), or when it fills. Events
  still in a live thread's partial chunk are lost only if the process dies first.
- Encoding stays on the drain thread.
