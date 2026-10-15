# TODO

Performance work on the trace ring backends (`ringbuf_mpsc`, `ringbuf_zcopy+mu`).
Measured with `bench_ring_mpsc` and `bench_ring_zcopy`; see
`BENCHMARK_ring_backends.md` for the baseline.

## 1. Shared counters on every event

Status: **step A done for `ringbuf_mpsc`; zcopy left as it was.** Each mpsc
counter (`published`, `dropped`, `consumed`, `retries`) now has its own cache
line. The same change to zcopy regressed its snapshot throughput by about 20%,
so zcopy's layout is unchanged until that is understood.

Results against the baseline in `BENCHMARK_ring_backends.md` (two runs each,
Mev/s):

| case | `ringbuf_mpsc` | `ringbuf_zcopy+mu` |
|---|---|---|
| lossless, 1 producer | 5.08 / 4.71 → 6.61 / 6.96 (+30–48%) | 9.42 / 8.48 → 11.08 / 8.78 (+4–18%) |
| lossless, 2 producers | 4.03 / 3.97 → 7.79 / 7.61 (+90%) | 5.96 / 5.89 → 6.28 / 6.34 (+5–8%) |
| lossless, 4 producers | 2.94 / 3.24 → 4.52 / 3.79 (+17–54%); drop-% 1.40 / 0.26 → 0.40 / 0.38 | 5.50 / 5.45 → 5.74 / 5.64 (+3–5%) |
| lossless, 7 producers | 2.02 / 2.00 → 3.04 / 2.99 (+50%) | 4.78 / 4.76 → 4.70 / 4.69 (flat) |
| snapshot, 2 producers | 6.62 / 7.02 → 6.56 / 6.21 (flat) | 5.37 / 5.29 → 4.18 / 2.75 (**−22 to −48%**) |
| snapshot, 4 producers | 5.47 / 5.21 → 5.11 / 4.96 (flat) | 5.31 / 5.22 → 4.30 / 4.16 (**−19 to −20%**) |
| snapshot, 4 producers, by ring size | 4.50–5.20 → 4.59–5.15 (flat) | 5.11–5.26 → 3.93–4.28 (**−18 to −25%**) |

Reading:

- **mpsc improves a lot in lossless mode.** At two producers the gain is
  large enough that the drain-side `consumed` sharing looks like the main
  cost, which is what the hypothesis predicted.
- **zcopy gains little in lossless mode and loses about 20% in snapshot mode.**
  The regression is consistent across both runs and every ring size, so it is
  not noise. The layout change moved the zcopy mutex and `pop_buf` relative to
  the counters. I have not identified which one causes it.

Next: find the zcopy snapshot cause before applying the same padding to zcopy.
Check whether the mutex now shares a line with the drain's `pop_buf`, or whether
the regression comes from the change in struct offsets alone. Then re-measure.

## 2. Per-thread rings

Each producer thread gets its own SPSC ring, so there is no CAS (mpsc), no mutex
(zcopy), and no retry loop. The drain thread merges the rings by timestamp. This
is how LTTng and ftrace structure per-CPU buffers.

Trade-off: a merge step in the drain, one buffer per thread, and a rule for
threads that exit with records still in their ring.

Do this only if item 1 does not close the gap.

## 3. Reader copy

Both backends pop each record into a private buffer before encoding it. Encoding
in place would save the 10–15 ns per event measured in [2] of the benchmark.

Trade-off: a slot stays reserved while it is being encoded, so producers see more
refusals when the ring is full.

## 4. Retry policy

`PF_RESERVE_SPINS` is 64 for both backends. The drop rates in the benchmark
suggest a value tuned per backend could reduce drops under contention. This is
the cheapest item to test.

## Notes

- Profiling: `perf c2c` would confirm cache-line contention, but it needs
  memory-event sampling, which often isn't available inside a VM. The benchmark
  A/B comparison does not depend on it.
