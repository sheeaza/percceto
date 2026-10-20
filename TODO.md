# TODO

Performance work on the trace ring backends (`ringbuf_mpsc`, `ringbuf_zcopy+mu`).
Measured with `bench_ring_mpsc` and `bench_ring_zcopy`; see
`tests/BENCHMARK_ring_backends.md` for the baseline.

## 1. Per-thread chunk buffers

Each producer thread writes into its own chunk, as Perfetto's `TraceWriter` does.
A chunk is acquired from a shared pool and handed to the drain when it is full
or flushed, so producers touch shared state only at chunk boundaries, not on
every event. This replaces the shared ring with no CAS (mpsc), no mutex
(zcopy), and no retry loop on the hot path.

Trade-off: the drain must read chunks from every thread, a rule is needed for
threads that exit with a partly filled chunk, and the pool size sets how much
data can be buffered before drops.

The benchmark shows the shared counters are the main per-event cost, so this is
the next step.

## 2. Reader copy

Both backends pop each record into a private buffer before encoding it. Encoding
in place would save the 10–15 ns per event measured in [2] of the benchmark.

Trade-off: a slot stays reserved while it is being encoded, so producers see more
refusals when the ring is full.

## 3. Retry policy (on hold)

`PF_RESERVE_SPINS` is 64 for both backends. The drop rates in the benchmark
suggest a value tuned per backend could reduce drops under contention. This is
the cheapest item to test.

## Notes

- Encoding stays on the drain thread. Moving it to the producer (as Perfetto
  does) was considered and dropped for now.
- Profiling: `perf c2c` would confirm cache-line contention, but it needs
  memory-event sampling, which often isn't available inside a VM. The benchmark
  A/B comparison does not depend on it.
