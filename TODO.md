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

## 3. Encode on the producer

Move protobuf encoding onto the producer, as Perfetto's SDK does: each record is
encoded into its chunk on the emitting thread, and the drain only copies chunk
bytes out. The drain's per-event work (`bench`: about 100 ns/event) then leaves
the drain thread, which is what limits the continuous-mode rate today.

Trade-off: the hot path gets more expensive, since encoding runs inside the
caller. Chunks become byte buffers with variable record sizes, so the
fixed-slot layout and `chunk_slots` as a record count no longer hold. Encoding
needs interned names and track ids available on the producer. Measure with
`bench_chunk` before and after.

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

## Notes

- Continuous mode has no timer flush. A thread's partial chunk is written when
  it unbinds (the thread exits, or closes the trace), or when it fills. Events
  still in a live thread's partial chunk are lost only if the process dies first.
- Encoding stays on the drain thread.
