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

## Notes

- Continuous mode has no timer flush. A thread's partial chunk is written when
  it fills, when the thread exits, or when the trace closes. Events still in a
  live thread's partial chunk are lost only if the process dies first.
- Encoding stays on the drain thread.
