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

## 2. Tails of live idle threads in dumps

Snapshot dumps are done. A dump walks every writer's partial chunk up to the
count the writer has published, so a live idle thread's records are in the
next dump. The cost is one release store per event, measured with
`bench_chunk` within noise.

Still open, in continuous mode and at close. A live thread's partial chunk is
written only when it fills, when the thread exits, or at close. Closing the gap
here needs the trace's lifetime managed by a refcount, so the last user flushes
it. That is a large change and is deferred.

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

## 5. Return a writer slot when its thread exits

A writer slot is not reclaimed when its thread exits, so a trace binds at most
`max_threads` threads over its life. Threads past that limit drop their events.
Option: once the exiting thread's partial chunk is committed, the slot goes back
to the unbound state so a later thread can take it. The slot's event count must
survive the reuse, or stats would lose the events already emitted. Only the
thread-exit destructor and bind touch `bound`, under `reg_lock`, so the hot path
does not change.

Trade-off: a slot is then reused by a different thread, so the writer's per-slot
state (`seq`, `cur`) has to be reset on bind, and a stale TLS cache in another
thread must still be rejected. Test with a trace that outlives more threads than
`max_threads`.

## 6. Replace the free list with a bitmap

The free list threads its chunks through their `next` fields, under `pool_lock`.
Option: keep one bit per pool chunk, indexed from the chunk's offset in
`pool_mem`. Taking a chunk is then a scan for a set bit, and returning one is a
bit set. Nothing is written into the chunk headers to track free chunks, and
the pool's invariants are easier to check.

Trade-off: the scan is linear in the pool size. At the default 8 MiB with 256
slots that is about 680 chunks, or 11 words, so it is cheap, but a large
`buffer_bytes` with small chunks would make it longer. A bitmap does not help
the committed FIFO, which still needs order. The `next` field is also borrowed
by the snapshot partial-chunk walk, so that walk may need a separate list after
this change.

## Notes

- Continuous mode has no timer flush. A thread's partial chunk is written when
  it fills, when the thread exits, or when the trace closes. Events still in a
  live thread's partial chunk are lost only if the process dies first.
- Encoding stays on the drain thread.
