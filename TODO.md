# TODO

Performance work on the trace ring backends (`ringbuf_mpsc`, `ringbuf_zcopy+mu`).
Measured with `bench_ring_mpsc` and `bench_ring_zcopy`; see
`tests/BENCHMARK_ring_backends.md` for the baseline.

## 1. Per-thread rings

Each producer thread gets its own SPSC ring, so there is no CAS (mpsc), no mutex
(zcopy), and no retry loop. The drain thread merges the rings by timestamp. This
is how LTTng and ftrace structure per-CPU buffers, and how Tracy's per-thread
queue works.

Trade-off: a merge step in the drain, one buffer per thread, and a rule for
threads that exit with records still in their ring.

The benchmark shows the shared counters are the main per-event cost, so this is
the next step.

## 2. Reader copy

Both backends pop each record into a private buffer before encoding it. Encoding
in place would save the 10–15 ns per event measured in [2] of the benchmark.

Trade-off: a slot stays reserved while it is being encoded, so producers see more
refusals when the ring is full.

## 3. Retry policy

`PF_RESERVE_SPINS` is 64 for both backends. The drop rates in the benchmark
suggest a value tuned per backend could reduce drops under contention. This is
the cheapest item to test.

## Notes

- Profiling: `perf c2c` would confirm cache-line contention, but it needs
  memory-event sampling, which often isn't available inside a VM. The benchmark
  A/B comparison does not depend on it.
