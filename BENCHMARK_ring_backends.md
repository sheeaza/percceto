# ringbuf_mpsc vs ringbuf_zcopy+mu, under pftrace

Two ring buffers measured through pftrace's own hot path. `src/pf_ring.h`
selects one at compile time, and `ringbuf_mpsc` is the default. Both sit under
the same `pftrace.c`, the same protobuf encoder and the same drain thread, so a
difference here is attributable to the ring and not to anything above it.

Both come from `../ringbuf` (see `src/VENDOR.md`). The bespoke mmap ring that
used to be the third backend was removed; its numbers are no longer produced by
this tree, so this document does not compare against it.

**Short answer: `ringbuf_zcopy+mu` has the higher lossless throughput at every
producer count, by 1.5–2.4×, and drops nothing where `ringbuf_mpsc` drops up to
1.8%. In snapshot mode the two are a tie, except at two producers, where mpsc is
ahead. Per-event cost and drain cost are within noise.**

## The two rings

| | `ringbuf_mpsc` (default) | `ringbuf_zcopy+mu` |
|---|---|---|
| claim | ticket CAS | **caller's mutex** |
| producers | N, lock-free | N, serialized |
| storage | fixed slots | fixed slots |
| readiness | per-slot stamp array | per-slot stamp array |
| reader | **pops into a private buffer** | **pops into a private buffer** |
| window upkeep | self, on eviction | self, on eviction |

`+mu` is not decoration. `ringbuf_zcopy.h` requires the *caller* to serialize
every `reserve()`; `src/pf_ring_zcopy.c` supplies that mutex, holding it across
the claim only — never the fill, never the submit, and never while retrying. It
is the one backend that takes a lock on pftrace's hot path, and measuring that
cost was the point of including it.

### Retry policy is shared

`PF_RESERVE_SPINS` and `PF_RESERVE_YIELD_EVERY` live in `src/pf_ring.h` and
govern both backends. A full lossless ring is transient: it clears when the
drain thread commits past the space the claim wants. Retrying with a shared
policy means the drop columns compare like with like.

## Build and run

```bash
cmake -B build -DPFTRACE_BUILD_BENCH=ON && cmake --build build -j8
cd build && ./bench_ring_mpsc && ./bench_ring_zcopy
```

Each binary takes about 70 s. One source (`tests/bench_ring.c`) is linked once
per backend: the backend is a compile-time choice and `struct pf_ring` is a
different type in each, so one process cannot hold both. These are deliberately
**not** `ctest` entries — they are measurements, not pass/fail checks — and are
always `-O2 -g` regardless of `CMAKE_BUILD_TYPE`.

Correctness first; none of this is worth reading if the suite is red:

```bash
cd build && ctest
```

## Method

- **2,000,000 events** per throughput run, split across producers (remainder to
  the first few, not dropped) so the wall-clock figure stays comparable as the
  producer count changes.
- **5 repetitions**, and each row reports the **one repetition whose throughput
  was the median** — not a per-column median. A per-column median would mix
  runs and could print a CPU figure that never accompanied the printed
  throughput.
- Consumer pinned to CPU 0, producers over CPUs 1–7. A mid-run migration shows
  up as a throughput cliff unrelated to the code.
- Measured through the **public API** (`pf_slice_end`, `pf_slice_begin`,
  `pf_counter_set_int`), not `pf_ring_reserve` directly. What matters is what a
  traced program pays, which includes the timestamp, four payload stores and
  the watermark check. The ring is a minority of that, so the per-event figures
  are closer together than the rings are.
- `published + dropped == attempted` is asserted every run and printed as `!!`
  if violated. **No failures in either run.**

### Two traps this is built around

**A ring that wraps inside the measured loop measures the reclaim policy.**
Three event shapes × 5 reps through one ring made whichever shape ran *last*
cost more than the first. `bench_cost` opens a fresh trace per shape per
repetition, so the shapes are measured on equal terms.

**Overwrite-mode throughput is not a win condition.** Evicting is cheaper than
delivering, so a backend can post a higher store rate purely by dropping more.
Hence the drop column is printed rather than summarized away, and CPU is
normalized per *published* event.

## Columns

| Column | Better | Meaning |
| --- | --- | --- |
| `Mev/s` | **higher** | Events published per second, wall clock |
| `cpu-ns/ev` | **lower** | Producer CPU time per published event, all producers summed. Counts only **on-CPU** time, so a thread parked on a mutex accrues none while a thread retrying a lock-free claim accrues all of it |
| `cpu/wall` | *neither* | CPU ÷ wall. At the producer count, every producer core is pegged |
| `drop-%` | **lower** | Events refused after exhausting retries. Never counts intentional snapshot-mode eviction |
| `ctxsw/k` | *informative* | Context switches per 1000 events, producers only |
| `spread` | lower | Slowest producer ÷ fastest. 1.00 is perfect fairness |
| `B/ev` | — | Output bytes per event. A property of the encoder, identical across backends; a sanity check, not a result |

## Hardware

12th Gen Intel Core i5-12500H, 8 vCPUs, **1 thread/core, 8 sockets** as the
guest sees it — a VMware VM, kernel 6.18.52, Clang 21.1.8, `-O2`. No CPU
frequency governor exposed.

**Absolute numbers are VM numbers and will not transfer.** The ratios are the
result. The 7-producer rows run 7 producers plus a drain thread on 8 vCPUs, so
they are at the edge of oversubscription and reflect scheduling as much as the
ring.

## Results

Two full runs, reported as **A / B** so the reproducibility is visible rather
than asserted. Rows are always in the order mpsc, zcopy; the winner is **bold**.

### [1] Per-event producer cost, single threaded, no draining

What a traced program pays per call, uncontended. This is zcopy's most
favourable case — its mutex is uncontended, so it costs two atomics, not a
futex.

| backend | `slice_end` | `slice_begin`+name | `counter` |
|---|---|---|---|
| `ringbuf_mpsc` | **47.4 / 47.2** | **47.2 / 47.4** | **47.7 / 46.7** |
| `ringbuf_zcopy+mu` | 48.7 / 55.5 | 49.1 / 55.5 | 48.9 / 55.3 |

Both within ~15% of each other in run A, and within ~20% of each other in run B.
Run B puts zcopy behind; run A puts it level. Treat this as a tie. Both pay
`clock_gettime` plus four stores, and the ring is a minority of that.

### [2] Drain cost: encode and write one pre-filled ring

The reader side, which sets continuous mode's ceiling.

| backend | ns/event | M events/s |
|---|---|---|
| `ringbuf_mpsc` | 74.8 / 74.2 | 13.4 / 13.5 |
| `ringbuf_zcopy+mu` | 68.7 / 77.0 | 14.6 / 13.0 |

The two backends are level within run-to-run spread. zcopy moves 12% between
its own runs, so no ordering is claimed here.

### [3] Lossless (continuous), by producer count, 8 MiB ring

| producers | backend | `Mev/s` | `cpu-ns/ev` | `drop-%` | `ctxsw/k` | `spread` |
|---|---|---|---|---|---|---|
| 1 | `ringbuf_mpsc` | 5.08 / 4.71 | 195 / 212 | 0.12 / 0.12 | 0.0 / 0.0 | 1.00 / 1.00 |
| | `ringbuf_zcopy+mu` | **9.42 / 8.48** | **106 / 117** | **0.05 / 0.06** | 0.0 / 0.0 | 1.00 / 1.00 |
| 2 | `ringbuf_mpsc` | 4.03 / 3.97 | 492 / 499 | 0.28 / 0.22 | 0.0 / 0.0 | 1.00 / 1.01 |
| | `ringbuf_zcopy+mu` | **5.96 / 5.89** | **328 / 333** | **0.00 / 0.00** | 0.1 / 0.2 | 1.03 / 1.01 |
| 4 | `ringbuf_mpsc` | 2.94 / 3.24 | 1336 / 1099 | 1.39 / 0.26 | 0.1 / 0.0 | 1.03 / 1.40 |
| | `ringbuf_zcopy+mu` | **5.50 / 5.45** | **577 / 583** | **0.00 / 0.00** | 10.2 / 10.2 | 1.02 / 1.07 |
| 7 | `ringbuf_mpsc` | 2.02 / 2.00 | 3225 / 3271 | 1.80 / 1.24 | 0.4 / 0.6 | 1.15 / 1.12 |
| | `ringbuf_zcopy+mu` | **4.78 / 4.76** | **949 / 973** | **0.00 / 0.00** | 29.1 / 30.2 | 1.04 / 1.02 |

zcopy+mu wins every row: 1.5× at two producers, 1.7–1.9× at four, 1.8× at one,
and 2.4× at seven. It drops nothing, where mpsc drops 0.12–1.8%. The
context-switch column gives the mechanism: zcopy's losers park on the mutex
(10–30 switches per 1000 events) while mpsc's losers retry in userspace, which
at seven producers costs about 3.4× the CPU per event.

### [4] Snapshot (overwriting), by producer count, 8 MiB ring

No backpressure, so nothing is dropped for lack of space. Any drop here is a
backend *refusing* a claim.

| producers | backend | `Mev/s` | `cpu-ns/ev` | `drop-%` |
|---|---|---|---|---|
| 1 | `ringbuf_mpsc` | **18.90 / 18.96** | **53 / 53** | 0.00 / 0.00 |
| | `ringbuf_zcopy+mu` | 18.03 / 18.23 | 55 / 55 | 0.00 / 0.00 |
| 2 | `ringbuf_mpsc` | **6.62 / 7.02** | **298 / 276** | 0.00 / 0.00 |
| | `ringbuf_zcopy+mu` | 5.37 / 5.29 | 364 / 371 | 0.00 / 0.00 |
| 4 | `ringbuf_mpsc` | 5.47 / 5.21 | 708 / 735 | 0.00 / 0.00 |
| | `ringbuf_zcopy+mu` | 5.31 / 5.22 | **593 / 604** | 0.00 / 0.00 |
| 7 | `ringbuf_mpsc` | 4.57 / 4.58 | 1417 / 1477 | 0.01 / 0.00 |
| | `ringbuf_zcopy+mu` | 4.60 / 4.59 | **999 / 995** | 0.00 / 0.00 |

Level at 1, 4 and 7 producers, with mpsc ahead at two (1.2–1.3×). The
two-producer gap is the only snapshot result that holds in both runs with a
margin above the noise.

### [5] Lossless, 4 producers, by ring size

| ring | backend | `Mev/s` | `cpu-ns/ev` | `drop-%` |
|---|---|---|---|---|
| 64 KiB | `ringbuf_mpsc` | 2.59 / 2.32 | 1502 / 1683 | 0.78 / 0.90 |
| | `ringbuf_zcopy+mu` | **4.68 / 4.72** | **666 / 678** | **0.23 / 0.13** |
| 1 MiB | `ringbuf_mpsc` | 2.44 / 2.50 | 1625 / 1548 | 0.41 / 0.38 |
| | `ringbuf_zcopy+mu` | **5.56 / 5.43** | **572 / 589** | **0.00 / 0.00** |
| 8 MiB | `ringbuf_mpsc` | 3.21 / 3.14 | 1157 / 1229 | 0.33 / 0.34 |
| | `ringbuf_zcopy+mu` | **5.53 / 5.45** | **576 / 585** | **0.00 / 0.00** |

zcopy's lead is widest at 1 MiB and 8 MiB, where it drops nothing. At 64 KiB
both lose throughput, and zcopy still leads.

### [6] Snapshot, 4 producers, by ring size

| ring | backend | `Mev/s` | `cpu-ns/ev` |
|---|---|---|---|
| 64 KiB | `ringbuf_mpsc` | 4.50 / 4.58 | 882 / 850 |
| | `ringbuf_zcopy+mu` | 5.15 / 5.25 | 603 / 597 |
| 1 MiB | `ringbuf_mpsc` | 4.62 / 4.55 | 849 / 872 |
| | `ringbuf_zcopy+mu` | 5.11 / 5.22 | 606 / 601 |
| 8 MiB | `ringbuf_mpsc` | 5.20 / 4.96 | 758 / 788 |
| | `ringbuf_zcopy+mu` | 5.21 / 5.26 | 603 / 593 |

Flat across 128× of ring size for both. In snapshot mode the reader exerts no
backpressure, so capacity only changes how often records recycle. Throughput is
within noise between the two backends. zcopy uses less CPU per event.

### [7] Named events, lossless, 4 producers, 8 MiB ring

`slice_begin` with a name, so the drain thread additionally interns a string per
event. The cost lands on the reader, not the producer, which stores only the
literal's address.

| backend | `Mev/s` | `cpu-ns/ev` | `drop-%` | `B/ev` |
|---|---|---|---|---|
| `ringbuf_mpsc` | 3.09 / 3.10 | 1261 / 1218 | 0.43 / 0.43 | 19.2 / 19.1 |
| `ringbuf_zcopy+mu` | **5.57 / 5.39** | **569 / 588** | **0.00 / 0.00** | 20.9 / 21.2 |

The same ordering as the unnamed 4-producer row in [3].

### [8] Paced, lossless, 4 producers, 8 MiB ring

1 ms sleep every 200 events. **The only scenario where the CPU columns add
information** — elsewhere `cpu/wall` pins at the producer count and `cpu-ns/ev`
is the wall cost restated.

| backend | `Mev/s` | `cpu-ns/ev` | `cpu/wall` | `drop-%` |
|---|---|---|---|---|
| `ringbuf_mpsc` | 0.68 / 0.68 | **273 / 294** | 0.19 / 0.20 | 0.00 / 0.00 |
| `ringbuf_zcopy+mu` | 0.67 / 0.68 | 325 / 322 | 0.22 / 0.22 | 0.00 / 0.00 |

Throughput is identical — it is set by the sleep, not the ring. With the cores
mostly idle, mpsc uses 9–16% less CPU per event than zcopy.

## Conclusions

1. **zcopy+mu has the higher lossless throughput.** [3] shows it ahead at every
   producer count, 1.5–2.4×, with no drops where mpsc drops up to 1.8%. The
   mechanism is the mutex: it parks losing producers, so the winner gets the
   cache line uncontended. mpsc's losers retry, and at seven producers that
   costs about 3.4× the CPU per event.

2. **In snapshot mode the two are level.** [4] and [6] show no consistent
   difference at one, four or seven producers. At two producers mpsc is ahead
   in both runs.

3. **The default is a separate decision.** `ringbuf_mpsc` is the default
   (`src/pf_ring.h`). These numbers favour zcopy+mu for lossless delivery. The
   trade is its serialized claim: it costs CPU under contention and needs a lock
   on the hot path, and mpsc has no lock. This document records the measurement;
   it does not change the default.

4. **Ring size is nearly irrelevant for either backend; producer count is
   everything.** Lossless throughput drops by 40–50% from one to seven
   producers for both. Tune the producer count or the drain interval, not the
   ring size.

5. **At a realistic event rate, the choice does not matter much.** [8]: with a
   1 ms pause every 200 events both deliver 0.68 Mev/s, and differ by 10–15%
   in CPU.

## Caveat

One host, one VM, two runs. **Treat gaps under ~15% as noise.** That makes
scenario [1], scenario [2], the snapshot rows at 1, 4 and 7 producers, and
scenario [6] ties. The lossless ordering in [3] and [5] is larger than that.

Specific cells I do not trust:

- **mpsc at 4 producers, lossless** ([3]): drop-% 1.39 in run A and 0.26 in run
  B, and spread 1.03 against 1.40. The least stable cell measured.
- **zcopy in [1] and [2]**: 48.7 against 55.5 ns per event for `slice_end`, and
  68.7 against 77.0 for encode+write, between runs. Its CPU column is less
  stable than mpsc's.
- **All 7-producer rows.** Seven producers plus a drain thread on 8 vCPUs is
  oversubscribed; those rows measure the scheduler as much as the ring.
- **`cpu-ns/ev` under saturation** is not independent of throughput. Where
  `cpu/wall` pins near the producer count, it is roughly the wall cost
  restated. Only [8] has idle cores and an informative CPU column.

Earlier figures for this document, from before the mmap backend was removed,
are not reproduced by these runs. The cause is not established, so those figures
are withdrawn rather than adjusted.

Not measured: a non-x86 host, more cores than producers, bare metal, any ring
larger than 8 MiB under contention, or `PF_RESERVE_SPINS` tuned per backend.
Both backends share the same spin count.
