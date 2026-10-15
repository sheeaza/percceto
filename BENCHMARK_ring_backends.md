# pf_ring vs ringbuf_mpsc vs ringbuf_zcopy, under pftrace

Three ring buffers measured through pftrace's own hot path. `src/pf_ring.h`
selects one at compile time; all three sit under the same `pftrace.c`, the same
protobuf encoder and the same drain thread, so a difference here is
attributable to the ring and not to anything above it.

None of the three is the baseline. `pf_ring` is the bespoke ring this library
was built on; the other two are general-purpose variants vendored from
`../ringbuf` (see `src/VENDOR.md`), and the question was whether either beats
the bespoke one in the program's real hot path.

**Short answer: no, and the margin is not close.** `pf_ring` wins every
throughput row. But the interesting results are the two that contradict the
reasoning that motivated the comparison, and they are in Conclusions 3 and 4.

## The three rings

| | `pf_ring` (mmap) | `ringbuf_mpsc` | `ringbuf_zcopy+mu` |
|---|---|---|---|
| claim | one `fetch_add` (snapshot) / CAS loop (lossless) | ticket CAS | **caller's mutex** |
| producers | N, lock-free | N, lock-free | N, serialized |
| storage | byte stream, double `mmap` | fixed slots | fixed slots |
| readiness | tag inside the record | per-slot stamp array | per-slot stamp array |
| reader | walks in place, seqlock | **pops into a private buffer** | **pops into a private buffer** |
| straddling records | contiguous via the mirror | n/a | n/a |
| window upkeep | caller: `trim` + `resync` | self, on eviction | self, on eviction |
| `pf_stats.resyncs` | can be nonzero | structurally 0 | structurally 0 |

`+mu` is not decoration. `ringbuf_zcopy.h` requires the *caller* to serialize
every `reserve()`; `src/pf_ring_zcopy.c` supplies that mutex, holding it across
the claim only — never the fill, never the submit, and never while retrying.
It is the one backend that takes a lock on pftrace's hot path, and pricing that
was the point of including it.

### What had to be equalized first

The first full run reported `pf_ring` at 12.18 Mev/s against `ringbuf_mpsc`'s
6.28 — while dropping **33% of events against 0.05%**. Both numbers were
accurate and the comparison was worthless: only the slot-indexed backends had a
retry loop, so it contrasted drop-on-full with retry-then-drop, and the
apparent winner was the backend discarding a third of its work.

The premise was also wrong. A full lossless ring is *transient* — it clears
when the drain thread commits past the space the claim wants, the same shape of
wait as a peer finishing a fill. So the retry policy belongs to the interface,
and `PF_RESERVE_SPINS` / `PF_RESERVE_YIELD_EVERY` now live in `src/pf_ring.h`
and govern all three. `pf_ring` at four producers then drops 2.0% instead of
60.6%, and the throughput columns mean something.

Every number below is from after that change.

## Build and run

```bash
cmake -B build -DPFTRACE_BUILD_BENCH=ON && cmake --build build -j8
cd build && ./bench_ring_mpsc && ./bench_ring_zcopy
```

~70 s per binary. One source (`tests/bench_ring.c`) linked three times: the
backend is a compile-time choice and `struct pf_ring` is a different type in
each, so one process cannot hold two. Deliberately **not** `ctest` entries —
measurements, not pass/fail checks — and always `-O2 -g` regardless of
`CMAKE_BUILD_TYPE`.

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
  in [1] are closer together than the rings are.
- `published + dropped == attempted` is asserted every run and printed as `!!`
  if violated. **No failures across either full run.**

### Two traps this is built around

**A ring that wraps inside the measured loop measures the reclaim policy.**
`tests/bench.c` already documented this; I hit it anyway. Three event shapes ×
5 reps = 15M events through one 512 MiB ring made whichever shape ran *last*
cost twice the first — `counter` at 102 ns against `slice_end`'s 47. Not a
property of counters: reordering the shapes in a scratch program moved the cost
with the order. `bench_cost` now opens a fresh trace per shape per repetition,
and the three agree to within a nanosecond.

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
than asserted. Rows are always in the order mmap, mpsc, zcopy regardless of who
won; the winner is **bold**.

### [1] Per-event producer cost, single threaded, no draining

What a traced program pays per call, uncontended. This is zcopy's most
favourable case — its mutex is uncontended, so it costs two atomics, not a
futex.

| backend | `slice_end` | `slice_begin`+name | `counter` |
|---|---|---|---|
| `pf_ring (mmap)` | 45.3 / 44.3 | 45.7 / 43.9 | 44.9 / 43.7 |
| `ringbuf_mpsc` | **41.5 / 43.2** | **43.2 / 42.7** | **42.5 / 42.2** |
| `ringbuf_zcopy+mu` | 48.3 / 49.6 | 48.1 / 48.6 | 47.9 / 48.3 |

All three within ~15%, and `ringbuf_mpsc` marginally ahead. At one producer a
ticket CAS and a `fetch_add` cost the same, and ~45 ns is dominated by
`clock_gettime` plus four stores, not by the ring. zcopy's uncontended mutex
costs ~5 ns.

### [2] Drain cost: encode and write one pre-filled ring

The reader side, which sets continuous mode's ceiling.

| backend | ns/event | M events/s |
|---|---|---|
| `pf_ring (mmap)` | **55.0 / 50.4** | 18.2 / 19.8 |
| `ringbuf_mpsc` | 70.7 / 64.8 | 14.1 / 15.4 |
| `ringbuf_zcopy+mu` | 61.5 / 61.3 | 16.2 / 16.3 |

The extra copy, priced: both ringbuf readers `pop` each record into a private
buffer, while the mmap reader encodes straight out of the mapping. That costs
**+10 to +15 ns per event**, or 20–28%.

### [3] Lossless (continuous), by producer count, 8 MiB ring

| producers | backend | `Mev/s` | `cpu-ns/ev` | `drop-%` | `ctxsw/k` |
|---|---|---|---|---|---|
| 1 | `pf_ring (mmap)` | **12.61 / 13.33** | **79 / 75** | 0.30 / 0.29 | 0.0 |
| | `ringbuf_mpsc` | 6.13 / 5.52 | 162 / 180 | 0.06 / 0.05 | 0.0 |
| | `ringbuf_zcopy+mu` | 7.39 / 6.86 | 134 / 145 | **0.02 / 0.02** | 0.0 |
| 2 | `pf_ring (mmap)` | **8.41 / 8.17** | **234 / 241** | 0.62 / 0.68 | 0.1 |
| | `ringbuf_mpsc` | 4.64 / 4.86 | 426 / 402 | 0.09 / 0.08 | 0.0 |
| | `ringbuf_zcopy+mu` | 6.39 / 6.33 | 307 / 297 | **0.00 / 0.00** | 0.1 |
| 4 | `pf_ring (mmap)` | **7.67 / 8.30** | **508 / 460** | 2.39 / 1.84 | 0.5 |
| | `ringbuf_mpsc` | 4.04 / 4.19 | 919 / 868 | 0.16 / 0.15 | 0.1 |
| | `ringbuf_zcopy+mu` | 5.76 / 5.97 | 550 / 539 | **0.00 / 0.00** | 9.4 / 8.6 |
| 7 | `pf_ring (mmap)` | **6.39 / 6.39** | 972 / 954 | 3.44 / 2.93 | 0.9 / 1.0 |
| | `ringbuf_mpsc` | 2.75 / 3.10 | 2368 / 2051 | 0.41 / 0.20 | 0.6 / 0.2 |
| | `ringbuf_zcopy+mu` | 4.78 / 4.63 | **987 / 1015** | **0.00 / 0.00** | 33.3 / 34.2 |

`pf_ring` wins throughput at every count, by 1.3–2.1×. But **zcopy beats
lock-free mpsc throughout** — 5.76 vs 4.04 at four producers — and drops
nothing at all. See Conclusion 3.

### [4] Snapshot (overwriting), by producer count, 8 MiB ring

No backpressure, so nothing is dropped for lack of space. Any drop here is a
backend *refusing* a claim, which `pf_ring` structurally cannot do.

| producers | backend | `Mev/s` | `cpu-ns/ev` | `drop-%` |
|---|---|---|---|---|
| 1 | `pf_ring (mmap)` | **30.05 / 32.43** | **33 / 31** | 0.00 |
| | `ringbuf_mpsc` | 21.40 / 20.72 | 46 / 48 | 0.00 |
| | `ringbuf_zcopy+mu` | 18.40 / 18.05 | 54 / 55 | 0.00 |
| 2 | `pf_ring (mmap)` | **17.29 / 15.39** | **111 / 128** | 0.00 |
| | `ringbuf_mpsc` | 6.71 / 6.64 | 294 / 296 | 0.00 |
| | `ringbuf_zcopy+mu` | 5.22 / 5.20 | 369 / 374 | 0.00 |
| 4 | `pf_ring (mmap)` | **21.63 / 21.20** | **175 / 178** | 0.00 |
| | `ringbuf_mpsc` | 4.73 / 4.89 | 832 / 806 | 0.01 / 0.00 |
| | `ringbuf_zcopy+mu` | 5.45 / 5.63 | 563 / 547 | 0.00 |
| 7 | `pf_ring (mmap)` | **20.41 / 19.63** | **300 / 300** | 0.00 |
| | `ringbuf_mpsc` | 4.27 / 4.27 | 1570 / 1573 | 0.00 |
| | `ringbuf_zcopy+mu` | 4.30 / 4.63 | **1061 / 994** | 0.00 |

The widest gaps in the whole benchmark: **4.3–4.8×** at 4–7 producers. This is
the `fetch_add` paying off — see Conclusion 2. The 2-producer dip for all three
reproduced in both runs and I have no confident explanation; it is named in the
Caveat.

The retries did their job: the slot-refusal drops I expected in snapshot mode
are 0.01% at worst.

### [5] Lossless, 4 producers, by ring size

| ring | backend | `Mev/s` | `cpu-ns/ev` | `drop-%` |
|---|---|---|---|---|
| 64 KiB | `pf_ring (mmap)` | **7.28 / 7.37** | **538 / 530** | 2.95 / 2.87 |
| | `ringbuf_mpsc` | 3.18 / 3.24 | 1226 / 1163 | 0.31 / 0.20 |
| | `ringbuf_zcopy+mu` | 4.58 / 4.47 | 695 / 739 | **0.06 / 0.09** |
| 1 MiB | `pf_ring (mmap)` | **7.46 / 8.22** | 528 / 474 | 2.73 / 2.20 |
| | `ringbuf_mpsc` | 3.34 / 3.24 | 1152 / 1203 | 0.24 / 0.18 |
| | `ringbuf_zcopy+mu` | 5.86 / 5.95 | **546 / 543** | **0.00 / 0.00** |
| 8 MiB | `pf_ring (mmap)` | **7.90 / 7.81** | 488 / 491 | 2.11 / 2.09 |
| | `ringbuf_mpsc` | 4.03 / 4.06 | 956 / 949 | 0.22 / 0.13 |
| | `ringbuf_zcopy+mu` | 6.04 / 5.97 | **537 / 541** | **0.00 / 0.00** |

Ring size barely matters for `pf_ring` (7.28 → 7.90, +9% across 128×) and
matters more for the slot-indexed pair, which lose ~25% at 64 KiB. Note zcopy's
`cpu-ns/ev` ties or beats `pf_ring`'s from 1 MiB up despite lower throughput —
the mutex parks its losers instead of spinning them.

### [6] Snapshot, 4 producers, by ring size

| ring | backend | `Mev/s` | `cpu-ns/ev` |
|---|---|---|---|
| 64 KiB | `pf_ring (mmap)` | **21.75 / 23.46** | **177 / 168** |
| | `ringbuf_mpsc` | 4.47 / 4.30 | 889 / 921 |
| | `ringbuf_zcopy+mu` | 5.35 / 5.63 | 576 / 546 |
| 1 MiB | `pf_ring (mmap)` | **23.04 / 23.17** | **170 / 170** |
| | `ringbuf_mpsc` | 4.35 / 4.34 | 912 / 911 |
| | `ringbuf_zcopy+mu` | 5.27 / 5.40 | 580 / 562 |
| 8 MiB | `pf_ring (mmap)` | **21.41 / 21.51** | **178 / 177** |
| | `ringbuf_mpsc` | 4.67 / 4.66 | 849 / 849 |
| | `ringbuf_zcopy+mu` | 5.44 / 5.65 | 569 / 549 |

Flat across 128× of ring size for all three. In snapshot mode the reader exerts
no backpressure, so capacity only changes how often records recycle — it does
not change the claim's cost. The clearest "this axis does not matter" result
here.

### [7] Named events, lossless, 4 producers, 8 MiB ring

`slice_begin` with a name, so the drain thread additionally interns a string per
event. Charges the reader, not the producer, which stores only the literal's
address.

| backend | `Mev/s` | `cpu-ns/ev` | `drop-%` | `B/ev` |
|---|---|---|---|---|
| `pf_ring (mmap)` | **7.43 / 7.33** | **524 / 523** | 2.55 / 2.53 | 21.2 |
| `ringbuf_mpsc` | 3.70 / 3.64 | 1005 / 1077 | 0.24 / 0.26 | 19.1 |
| `ringbuf_zcopy+mu` | 5.91 / 5.98 | 547 / 542 | **0.00 / 0.00** | 20.9 |

Within 3% of the unnamed 4-producer row in [3] for every backend. Storing the
name by address rather than copying it is doing its job, and it does so
identically on all three.

### [8] Paced, lossless, 4 producers, 8 MiB ring

1 ms sleep every 200 events. **The only scenario where the CPU columns add
information** — elsewhere `cpu/wall` pins at the producer count and
`cpu-ns/ev` is the wall cost restated.

| backend | `Mev/s` | `cpu-ns/ev` | `cpu/wall` | `drop-%` |
|---|---|---|---|---|
| `pf_ring (mmap)` | 0.71 / 0.70 | **138 / 164** | 0.10 / 0.12 | 0.00 |
| `ringbuf_mpsc` | 0.71 / 0.70 | 155 / 183 | 0.11 / 0.13 | 0.00 |
| `ringbuf_zcopy+mu` | 0.69 / 0.69 | 238 / 257 | 0.17 / 0.18 | 0.00 |

Throughput is identical — it is set by the sleep, not the ring. With the cores
idle (`cpu/wall` ~0.1) the spread narrows to 138/155/238 ns: **at a realistic
event rate the choice of ring is nearly irrelevant**, and zcopy's mutex is the
only one that still shows, at ~1.7× the CPU.

## Conclusions

1. **`pf_ring` should stay.** It wins every throughput row — 1.3–2.1× in
   lossless mode, 4.3–4.8× in snapshot at 4+ producers — and has the lowest
   producer CPU nearly everywhere. The bespoke ring earns its place, and
   neither general-purpose variant is a reason to replace it.

2. **The win is the storage model, not the claim.** [1] shows all three within
   15% at one producer, so the claim mechanisms cost about the same. The gap
   appears only with contention and is widest in snapshot mode, where
   `pf_ring`'s reserve is an unconditional `fetch_add` with no validation, no
   per-slot stamp to acquire-load, and no CAS to lose. The slot-indexed pair
   must check a stamp and win a ticket even when the ring is nowhere near full.

3. **A caller-held mutex beat a lock-free CAS, which is the opposite of what I
   expected.** `ringbuf_zcopy+mu` beats `ringbuf_mpsc` in *every* multi-producer
   lossless row — 5.76 vs 4.04 Mev/s at four producers, and 4.78 vs 2.75 at
   seven — and drops **nothing at all** where mpsc drops 0.15–0.41%. The
   mechanism is visible in `ctxsw/k`: 9.4 against 0.1. The mutex parks a losing
   producer, so the winner gets the cache line uncontended and the loser burns
   no cycles; mpsc's losers retry in userspace, and at seven producers that
   costs 2368 cpu-ns/ev against zcopy's 987. Serializing claims is *cheaper*
   than contending for them here, which is why upstream calls this a split
   decision rather than a ranking.

4. **The extra reader copy costs 20–28%, and it buys something.** [2]: 70.7 and
   61.5 ns/event against the mmap reader's 55.0. But both ringbuf readers hold a
   private copy, so nothing can lap a record mid-encode — which is why they need
   no seqlock, make `pf_stats.resyncs` structurally 0, and run clean under
   ThreadSanitizer **with no suppression file at all**, where the mmap ring
   needs four entries for snapshot mode's deliberate overwrite race. A real
   safety gain for a real throughput cost.

5. **Ring size is nearly irrelevant; producer count is everything.** 128× of
   capacity moves `pf_ring` by 9% in lossless mode ([5]) and not at all in
   snapshot ([6]). Going from 1 to 7 producers costs it half its throughput.
   Tune the producer count, or the drain interval; do not tune `ring_bytes` for
   speed.

6. **At a realistic event rate, none of this matters.** [8]: with a 1 ms pause
   every 200 events all three deliver 0.70 Mev/s and differ only in CPU, where
   the worst is 1.7× the best on cores that are 90% idle. These backends diverge
   under saturation, which is a load a tracer should not be under.

7. **The seam was worth building regardless of the verdict.** Four accessors
   (`capacity_slots`, `occupancy_slots`, `stats`, `reader_may_be_lapped`)
   replaced every direct field read, three backends implement one interface,
   and `ctest` went from 7 tests to 11 because `test_stress` and `test_noalloc`
   use the public API only and so are a correctness gate for any ring. The cost
   was ~0 ns/event: [1] matches the pre-seam figure.

### Caveat

One host, one VM, two runs. **Treat gaps under ~15% as noise** — which makes
all of scenario [1] a tie and leaves every ratio in [3]–[6] comfortably real.

Specific cells I do not trust:

- **The 2-producer dip in [4].** All three backends are slower at 2 producers
  than at 4 (`pf_ring` 17.29 vs 21.63 Mev/s). It reproduced in both runs, so it
  is not sampling noise, but I have no mechanism for it. Possibly vCPU topology
  — the guest reports 8 single-thread sockets, which is not real hardware.
- **`ringbuf_mpsc` at 1 producer, lossless** ([3]): 6.13 then 5.52, a 10% swing
  between runs, the least stable cell measured.
- **All 7-producer rows.** 7 producers plus a drain thread on 8 vCPUs is
  oversubscribed; those rows measure the scheduler as much as the ring.
- **`cpu-ns/ev` under saturation** is not independent of throughput. Where
  `cpu/wall` pins near the producer count, it is roughly the wall cost
  restated — only [8] has idle cores and therefore an informative CPU column.

Not measured: a non-x86 host, more cores than producers, bare metal, any ring
larger than 8 MiB under contention, or `PF_RESERVE_SPINS` tuned per backend
(all three share 64, which [3]'s drop columns suggest suits zcopy best).
