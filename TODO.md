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

A thread that has emitted but not filled its chunk, and is still alive, is not
in a snapshot dump. Its records appear after it emits again, fills its chunk,
or exits. Options: a per-event shared store (rejected for the hot path), or
have the dump stall until each live writer acknowledges a flush request.

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

## Notes

- Continuous mode has no timer flush. A thread's partial chunk is written when
  it fills, when the thread exits, or when the trace closes. Events still in a
  live thread's partial chunk are lost only if the process dies first.
- Encoding stays on the drain thread.
