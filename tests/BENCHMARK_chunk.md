# Chunk buffer benchmark

Producer cost of the chunk buffers, from `tests/bench_chunk.c`. Run with
`../bench_chunk` from `build/run`.

## Setup

- 8 cores online.
- Snapshot mode, 8 MiB pool, so the pool wraps and every rotation is a
  steady-state steal under the pool lock. No drain, no drops.
- Each event is a `pf_slice_begin` / `pf_slice_end` pair; ns/event counts
  records, so one pair is two events.
- The library's `clock_gettime` is stubbed with a thread-local counter, as
  `bench_args` does. A raw `clock_gettime` costs about 8.5 µs on this machine,
  which would otherwise dominate every number.
- Single runs. Expect a few ns of run-to-run noise.

## Rotation sweep, 1 thread

`chunk_slots` is records per chunk.

| chunk_slots | before alignment | after alignment |
|---:|---:|---:|
| 16   | 6.2 ns/event | 5.3 ns/event |
| 64   | 6.5 ns/event | 4.7 ns/event |
| 256  | 4.7 ns/event | 6.4 ns/event |
| 1024 | 5.9 ns/event | 5.9 ns/event |
| 4096 | 6.4 ns/event | 6.0 ns/event |

Rotation cost is not visible in the chunk size: all sizes fall within the
noise band.

## Scaling, chunk_slots 256

Per-thread ns/event, with aggregate M events/s in brackets.

| threads | before alignment | after alignment |
|---:|---:|---:|
| 1 | 4.5 ns (221 M/s)  | 6.3 ns (158 M/s)  |
| 2 | 17.3 ns (115 M/s) | 7.8 ns (256 M/s)  |
| 4 | 19.0 ns (162 M/s) | 10.0 ns (367 M/s) |
| 8 | 30.0 ns (214 M/s) | 21.7 ns (352 M/s) |

"Before" is the writer struct without `aligned(64)`. "After" adds
`aligned(64)` to `pf_writer` and allocates the writers array with
`aligned_alloc(64, …)`. The gain shows from two threads up: the unaligned
writers share cache lines, so each producer's hot fields bounce between cores.
One thread looks slower after alignment (4.5 → 6.3 ns). That gap is larger
than the run-to-run noise seen elsewhere in this table, so it is not explained
yet. Single-thread runs were not repeated to check it.

## Per-trace TLS slots

`pf_tls` holds one slot per open trace, indexed by the trace's index, so the
writer lookup is a TLS base, an index load and a compare. Measured against HEAD,
three interleaved runs each, Release builds, `chunk_slots` 256, ns/event:

| threads | HEAD (single TLS slot) | per-trace slots |
|---:|---:|---:|
| 1 | 5.0–6.6 | 5.4–7.0 |
| 8 | 24.7–26.5 | 23.9–26.1 |

At 8 threads the two are within noise. At 1 thread the new build is about 0.5–1
ns slower on these runs, which is about the size of the noise, so the cost is not
settled.
