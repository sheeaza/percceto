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

## 4. Chunk boundaries may share a cache line

The writers array is cache-line aligned (`pf_writer` is `aligned(64)`), which
cut the 8-thread producer cost from about 30 to 22 ns/event in `bench_chunk`.
Pool chunks are only 8-byte aligned, so the last line of one chunk can share a
line with the first line of the next. Each chunk has one owner while it fills,
so this should matter only at the boundary. Not measured.

Option: round `chunk_bytes` up to a multiple of 64 and align the pool, at the
cost of padding each chunk. Measure with `bench_chunk` first.

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

## 7. Let a thread bind to more than one trace (low priority)

A thread's binding (trace, writer) is one TLS slot, so a thread binds one trace
at a time. Binding a second trace unbinds the first: its partial chunk is
committed and its reference dropped, and the thread's later events go to the
second trace only. The simple API keeps the thread's track uuid in one TLS slot
too, so it has the same limit.

Option: model the bindings as per-thread maps, a thread id to writer and a
thread id to track uuid, so one thread holds a writer and a uuid in each trace
it uses. The hot path must stay a TLS read and a compare. So the map would be a
small per-thread cache in front of the table, touched only on bind and on
thread exit. Measure with `bench_chunk` before and after.

## Notes

- Continuous mode has no timer flush. A thread's partial chunk is written when
  it unbinds (the thread exits, or closes the trace), or when it fills. Events
  still in a live thread's partial chunk are lost only if the process dies first.
- Encoding stays on the drain thread.
