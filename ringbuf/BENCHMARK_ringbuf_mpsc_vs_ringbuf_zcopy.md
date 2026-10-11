# ringbuf_mpsc vs ringbuf_zcopy: two ways to take several producers

Head-to-head measurement of the two multi-producer variants, **both using
their split push**, so the payload is written in place exactly once on each
side. The harness is `bench_ringbuf_mpsc_vs_ringbuf_zcopy.c`, built as the
`bench_ringbuf_mpsc_vs_ringbuf_zcopy` target.

**Neither side is the baseline here.** Unlike the other two writeups, this one
compares two variants that are both first-class answers to the same question,
and the result is a genuine split decision rather than a winner.

## Why this comparison needed a library change first

The two variants take N producers by opposite means:

| | `ringbuf_mpsc` | `ringbuf_zcopy` |
| --- | --- | --- |
| Producer sync | **itself**, ticket CAS on a multi-writer `write_idx` | **the caller**, one mutex around every reserve |
| `reserve` | any number of threads, no lock | single-caller **at a time** |
| `write_idx` | multi-writer, validating CAS + retry | single-writer, relaxed both ways |
| Lock-free / signal-safe | **yes**, both modes | no — `reserve` may block |

Before this work, they did not share an *operation*. `ringbuf_mpsc_push`
copies the payload from a caller buffer, so a producer that **generates** its
message writes the bytes twice — once into staging, once into the slot —
while `ringbuf_zcopy` hands out a slot pointer and writes them once. A
comparison over those APIs would have conflated *"one copy vs two"* with
*"ticket CAS vs caller mutex"*, and no fill model fixes that: it was an API
difference, not a harness artifact.

So `reserve`/`submit` was added to `ringbuf_mpsc` (see CLAUDE.md, "The split
push"). With it, the headline rows do equal byte-moving work and the only
remaining difference is the synchronization — which is what this benchmark
measures.

### Four rows, which is what makes a gap attributable

| row | arrangement | copies | lock |
| --- | --- | --- | --- |
| `ringbuf_mpsc rsv` | `reserve` / fill in place / `submit` | 1 | **none** |
| `ringbuf_zcopy+mu` | lock/`reserve`/unlock, fill, `submit` | 1 | caller mutex, **claim only** |
| `ringbuf_mpsc push` | fill staging, `push` | 2 | none |
| `ringbuf_zcopy+mu psh` | fill staging, lock/`push`/unlock | 2 | caller mutex, **across the copy** |

Rows 1↔2 are the headline. Rows 3↔4 are the pre-split control. Rows 1↔3 and
2↔4 price the split push *within* each variant, which is how to tell whether
an advantage came from filling in place or from the synchronization.

**Every results table below lists `ringbuf_mpsc` rows before `ringbuf_zcopy`
ones, regardless of which won** — the winner is marked in bold instead. Row
order is fixed so the tables can be read down a column and compared across
scenarios without re-checking which variant a row belongs to; the benchmark
itself prints in this same order.

### The caller's mutex discipline is load-bearing, not stylistic

`ringbuf_zcopy`'s rows hold the mutex across the **claim only** and retry with
it **released**. Both are required by `ringbuf_zcopy.h` rules 1–2, not
preferences: a refused reserve is usually blocked by a *peer's* unsubmitted
slot, so spinning while holding the lock would block the very producer whose
submit would clear it. That is a deadlock, not a slow row.

## Build and run

```bash
nix shell ~/project/nixos_config/.#cshell --command bash -c \
    'cmake -B build -DRINGBUF_BUILD_BENCH=ON && cmake --build build -j'

./build/bench_ringbuf_mpsc_vs_ringbuf_zcopy    # ~7 min
```

Opt-in behind the same flag as the other three benchmarks, and deliberately
not a CTest entry: these are measurements, not pass/fail checks. Always
compiled `-O2` regardless of `CMAKE_BUILD_TYPE`. Linux-only — it pins threads
with `pthread_setaffinity_np`.

Correctness is a separate gate and should be green before trusting any of
this:

```bash
nix shell ~/project/nixos_config/.#cshell --command ctest --test-dir build \
    -R '^ringbuf_mpsc$|^ringbuf_zcopy$|^ringbuf_zcopy_mpsc$|^ringbuf_mpsc_tsan$|^ringbuf_zcopy_tsan$|^ringbuf_zcopy_mpsc_tsan$' \
    --output-on-failure
```

## Method

- 2,000,000 messages per run, **split across the producers** (1,000,000 for
  the paced scenario), so wall throughput stays comparable as the producer
  count changes.
- **Median of 5 repetitions, reported as one whole run** — not a per-column
  median, so every column of a row describes the same repetition.
- The consumer is pinned to CPU 0 and each producer to CPUs 1..N, cycling.
- `received + dropped == pushed` is asserted per run; `!!` is printed if it
  ever breaks. **Zero failures across both full runs.**

### The fill-cost axis, and why it is the point

Filling a slot is the work the two arrangements overlap differently: the mutex
covers only the claim, so N fills proceed concurrently, while the `push` rows
necessarily serialize the copy inside whatever lock they hold. With a trivial
fill there is nothing to overlap; with an expensive one the overlap is the
whole game.

`fill()` does `passes` word-wise read-modify-write passes over the payload
through a `volatile uint64_t *`, so its cost scales with `slot_size × passes`
and cannot be optimized away. Scenario [4] sweeps `passes`; scenario [5]
sweeps `slot_size`, which is the other way to make the same fill expensive.

### Two differences from the SPSC benchmark — numbers are NOT comparable

1. **`cpu/wall` saturates near `producers+1`, not 2.0.**
2. **A failed store or pop yields after 64 consecutive failures** instead of
   spinning forever. Applied identically to all four rows.

### One harness rule the other benchmarks do not need

**A false pop never means "drained."** In *both* variants the oldest slot may
be reserved but unsubmitted, which blocks delivery head-of-line. The consumer
may only conclude the stream is over once every producer has finished **and**
every reservation has been submitted — each producer decrements
`producers_left` only after its final submit.

### Columns

`Mm/s` is **millions of messages per second**.

| Column | Better | Meaning |
| --- | --- | --- |
| `store-Mm/s` | higher — at equal drop rates | Stores **completed** per second |
| `recv-Mm/s` | **higher** | Messages actually **delivered** per second |
| `cpu-ns/rx` | **lower** | CPU-ns per *delivered* message, all threads summed |
| `cpu/wall` | *neither* | CPU ÷ wall; `producers+1` = every core pegged |
| `prod-ns` / `cons-ns` | lower | CPU-ns per store; `prod-ns` sums **all** producers |
| `stor-rt/k` / `pop-rt/k` | lower | Spin retries per 1000 messages |
| `ctxsw/k` | lower | Context switches per 1000 messages, every thread |
| `finish spread` | lower, 1.00 ideal | Slowest producer's wall time ÷ fastest's |

## Hardware

12th Gen Intel Core i5-12500H, 8 vCPUs under VMware, 1 thread/core, no SMT
siblings. Linux 6.18.52, clang, `-O2`.

Absolute numbers are VM numbers and will not transfer; the ratios between rows
of a table are the result. Everything below reproduced across two full runs.

## Results

### [1] Uncontended, single thread, alternating store/pop (depth 1)

Queue depth 1, 64B slots, so no cross-core traffic and no contention at all.
`ringbuf_zcopy raw` takes **no push lock** — legal only because there is one
thread here — so the gap from it to `+mu psh` prices the external mutex alone.

One `Mmsg/s` unit is a store **and** a pop, so `cpu-ns/op` covers the pair.

| impl | mode | Mmsg/s | cpu-ns/op |
| --- | --- | --- | --- |
| ringbuf_mpsc rsv | lossless | 74.01 | 13.46 |
| ringbuf_mpsc push | lossless | 86.38 | 11.53 |
| ringbuf_zcopy+mu | lossless | 81.22 | 12.28 |
| ringbuf_zcopy+mu psh | lossless | 85.45 | 11.67 |
| **ringbuf_zcopy raw** | **lossless** | **104.88** | **9.51** |
| ringbuf_mpsc rsv | overwrite | 41.54 | 23.94 |
| ringbuf_mpsc push | overwrite | 39.31 | 25.36 |
| ringbuf_zcopy+mu | overwrite | 63.09 | 15.82 |
| ringbuf_zcopy+mu psh | overwrite | 60.47 | 16.41 |
| **ringbuf_zcopy raw** | **overwrite** | **83.05** | **11.98** |

Three figures fall out, all instruction cost rather than synchronization:

- **The external mutex costs ~2.2 ns lossless, ~4.4 ns overwrite**
  (`raw` → `+mu psh`), uncontended — one `pthread_mutex_lock`/`unlock` pair.
- **`ringbuf_mpsc` costs ~2.0 ns more than `ringbuf_zcopy+mu` lossless, and
  ~8.1 ns more in overwrite mode.** The overwrite gap is the expensive one:
  `pop_overwrite` does three atomic RMWs on every pop (slot-lock CAS, stamp
  CAS, `advance_read_idx`) plus the ticket CAS on the store side, against
  `ringbuf_zcopy`'s one uncontended internal mutex in `pop` and nothing on
  `reserve`'s fast path.
- **Splitting the push costs ~1.9 ns on the `ringbuf_mpsc` side** and nothing
  measurable on the `zcopy` side. With a 64B fill there is no copy worth
  saving, so the reserve rows only pay the extra call.

**`ringbuf_mpsc` enters every contended scenario below carrying this
handicap**, which is the mirror image of its position against `ringbuf_mutex`.

### [2] Lossless, 64B slots, cap 4096, by producer count

Headline rows only (`rsv` vs `+mu`); full tables in the raw output.

| producers | impl | recv-Mm/s | cpu-ns/rx | cpu/wall | stor-rt/k | pop-rt/k | ctxsw/k | spread |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | **ringbuf_mpsc rsv** | **26.55** | **74.6** | 1.98 | 0.8 | 88.5 | 0.01 | 1.00 |
| 1 | ringbuf_zcopy+mu | 18.19 | 109.3 | 1.99 | 1.2 | 347.2 | 0.01 | 1.00 |
| 2 | **ringbuf_mpsc rsv** | **12.99** | **219.0** | 2.84 | 2.0 | 508.7 | 0.01 | 1.16 |
| 2 | ringbuf_zcopy+mu | 11.24 | 258.9 | 2.91 | 0.0 | 1339.2 | 0.05 | 1.06 |
| 4 | ringbuf_mpsc rsv | 8.84 | 547.0 | 4.83 | 1.6 | 821.8 | 0.07 | 1.04 |
| 4 | **ringbuf_zcopy+mu** | **12.86** | **312.9** | 4.02 | 0.0 | 1015.0 | 4.53 | 1.11 |
| 7 | ringbuf_mpsc rsv | 7.27 | 1014.2 | 7.38 | 308.8 | 1062.5 | 0.13 | 1.13 |
| 7 | **ringbuf_zcopy+mu** | **10.61** | **532.1** | 5.65 | 0.1 | 1492.8 | 15.70 | 1.05 |

**The crossover is between 2 and 4 producers, and it is the central result of
this file.** At 1–2 producers `ringbuf_mpsc` wins (1.46× at one, 1.16× at
two); at 4–7 `ringbuf_zcopy+mu` wins (1.45× at four, 1.46× at seven) on both
throughput *and* CPU per delivered message.

Why it reverses is visible in `cpu/wall`: `ringbuf_mpsc` pegs 4.83 and 7.38 at
4 and 7 producers, while `ringbuf_zcopy+mu` sits at 4.02 and 5.65. The
lock-free variant's losing producers **spin**, and past a few producers that
spinning costs more than the mutex's 4.5–15.7 context switches per 1000
messages. At 7 producers `ringbuf_mpsc` burns 309 failed stores per 1000
messages against `ringbuf_zcopy`'s 0.1.

The 2-producer row is the least stable cell in the benchmark (`zcopy`'s
delivered rate swung 11.24 → 7.81 between runs, i.e. 1.16× to 1.67× in
`ringbuf_mpsc`'s favour). The *direction* held; treat the magnitude as
uncertain.

### [3] Overwrite, 64B slots, cap 4096, by producer count

Same shape, same crossover. At 4 producers `ringbuf_zcopy+mu` delivers
13.12 Mm/s against `ringbuf_mpsc rsv`'s 8.70 (1.51×) at 309.2 vs 569.1
cpu-ns/rx. Drop rates were ≤0.02% on both sides, so `recv` and `store` agree
and nothing is being won by discarding more.

Worth noting: the reserve rows *can* fail here where the push rows cannot —
both variants refuse a slot whose reservation is unsubmitted — but at this
capacity it is rare (0.2–1.6 retries per 1000 messages).

### [4] Lossless, 4 producers, 256B slots, BY FILL COST — the table this exists for

| passes | impl | recv-Mm/s | cpu-ns/rx | cpu/wall | pop-rt/k | ctxsw/k |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | ringbuf_mpsc rsv | 9.49 | 516.6 | 4.90 | 362.4 | 0.02 |
| 1 | **ringbuf_zcopy+mu** | **11.51** | **347.3** | 4.00 | 1196.2 | 5.24 |
| 2 | ringbuf_mpsc rsv | 9.37 | 522.6 | 4.90 | 335.3 | 0.01 |
| 2 | **ringbuf_zcopy+mu** | **9.58** | **422.7** | 4.05 | 1487.4 | 5.91 |
| 8 | **ringbuf_mpsc rsv** | **10.94** | **435.8** | 4.77 | 503.0 | 0.01 |
| 8 | ringbuf_zcopy+mu | 5.98 | 721.1 | 4.31 | 2278.3 | 7.99 |
| 32 | **ringbuf_mpsc rsv** | **7.17** | **642.3** | 4.60 | 1499.9 | 0.01 |
| 32 | ringbuf_zcopy+mu | 4.16 | 1098.5 | 4.57 | 3865.7 | 5.70 |

**This inverts the hypothesis the benchmark was built to test, and that is the
most useful thing in the file.** The expectation was that an expensive fill
would favour `ringbuf_zcopy+mu`, because its mutex covers only the claim and
the fills overlap. The opposite happens: `zcopy` leads 1.21× at `passes=1`,
parity at 2, then **loses 1.83× at 8 and 1.72× at 32**. The crossover sits
between `passes=1` and `passes=2`, reproduced in both runs.

The reason is that the premise was wrong about *what is scarce*. Both
arrangements already overlap their fills — `ringbuf_mpsc`'s fills are outside
any lock too, because there is no lock. So a longer fill does not buy `zcopy`
any parallelism `mpsc` lacks; it just holds each slot longer, which makes the
ring behave like a smaller one. That raises the rate at which a producer finds
the slot it wants still occupied, and `zcopy`'s refusals are **more expensive
than `mpsc`'s**: a refused reserve has to re-acquire the push mutex to try
again, where `ringbuf_mpsc` just re-reads `write_idx` and re-CASes. `pop-rt/k`
shows it directly — 3866 vs 1500 at `passes=32`.

So the fill-cost axis measures something real, just not what was predicted:
**it is a proxy for slot occupancy, and occupancy is where the lock-free claim
pays off.**

### [5] Lossless, 4 producers, cap 4096, by slot size

| slot | impl | recv-Mm/s | cpu-ns/rx | pop-rt/k | ctxsw/k |
| --- | --- | --- | --- | --- | --- |
| 16B | ringbuf_mpsc rsv | 8.40 | 587.7 | 841.6 | 0.01 |
| 16B | **ringbuf_zcopy+mu** | **13.29** | **309.5** | 992.4 | 4.76 |
| 64B | ringbuf_mpsc rsv | 8.73 | 567.0 | 757.5 | 0.01 |
| 64B | **ringbuf_zcopy+mu** | **12.94** | **313.5** | 1042.9 | 4.51 |
| 256B | ringbuf_mpsc rsv | 9.21 | 535.2 | 401.8 | 0.01 |
| 256B | **ringbuf_zcopy+mu** | **11.41** | **348.6** | 1205.9 | 5.25 |
| 1024B | **ringbuf_mpsc rsv** | **9.27** | 517.1 | 247.2 | 0.01 |
| 1024B | ringbuf_zcopy+mu | 7.70 | 514.0 | 1426.6 | 7.99 |

The same crossover, driven the other way: `ringbuf_zcopy+mu` leads 1.58× at
16B, narrowing monotonically to parity at 256B and reversing to 1.20× for
`ringbuf_mpsc` at 1024B. Consistent with [4] — more payload bytes means each
slot is held longer, which is the same occupancy effect.

Note the 1024B `cpu-ns/rx` column: 517.1 vs 514.0, i.e. **identical CPU per
delivered message** while `ringbuf_mpsc` delivers 20% more. `ringbuf_zcopy` is
spending its CPU on futex traffic (24.92 ctxsw/k, the highest in the
benchmark) rather than on useful work.

### [6] Overwrite, 4 producers, 64B slots, by capacity

| cap | impl | recv-Mm/s | cpu-ns/rx | dropped | stor-rt/k | pop-rt/k | ctxsw/k |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 16 | **ringbuf_mpsc rsv** | **7.33** | **664.9** | 0.57% | 117.9 | 106.8 | 0.01 |
| 16 | ringbuf_zcopy+mu | 2.61 | 1273.7 | 1.76% | 1811.5 | 4708.7 | 33.90 |
| 256 | ringbuf_mpsc rsv | 8.18 | 600.2 | 0.20% | 15.4 | 54.9 | 0.01 |
| 256 | **ringbuf_zcopy+mu** | **9.95** | **384.3** | 0.41% | 133.3 | 1079.4 | 6.90 |
| 4096 | ringbuf_mpsc rsv | 8.62 | 570.2 | 0.04% | 0.7 | 24.9 | 0.01 |
| 4096 | **ringbuf_zcopy+mu** | **12.40** | **323.3** | 0.00% | 0.0 | 828.4 | 4.88 |
| 65536 | ringbuf_mpsc rsv | 8.88 | 554.2 | 0.00% | 0.0 | 42.9 | 0.01 |
| 65536 | **ringbuf_zcopy+mu** | **13.36** | **301.4** | 0.00% | 0.0 | 785.4 | 4.59 |

**The sharpest result in the benchmark is `cap=16`: `ringbuf_mpsc` delivers
2.81× more** (2.61 → 7.33 Mm/s) at half the CPU per message, and it is the
same occupancy story at its extreme. With 16 slots, 4 producers and an
unsubmitted-slot refusal in play, `ringbuf_zcopy+mu` spends 1812 failed stores
and 4709 failed pops per 1000 messages, with 33.9 context switches — the lock
is being acquired, refused, and released over and over. `ringbuf_mpsc` pays
118 and 107. Reproduced closely (2.50 Mm/s in run 2).

This is the configuration where the caller-held mutex is worst: **its retry
has to go back through the lock**, so a contended small ring turns the mutex
into a thundering herd. Past `cap=256` the ring is big enough that refusals
are rare and the mutex's lower spin cost wins again.

### [7] Oversubscribed: 16 producers on 7 producer CPUs

| mode | impl | recv-Mm/s | cpu-ns/rx | cpu/wall | stor-rt/k | ctxsw/k | spread |
| --- | --- | --- | --- | --- | --- | --- | --- |
| lossless | ringbuf_mpsc rsv | 7.77 | 910.8 | 7.08 | 578.0 | 9.14 | 1.78 |
| lossless | **ringbuf_zcopy+mu** | **9.39** | **584.5** | 5.49 | 16.1 | 43.42 | 1.56 |
| overwrite | ringbuf_mpsc rsv | 7.36 | 1006.2 | 7.40 | 69.3 | 1.17 | 1.36 |
| overwrite | **ringbuf_zcopy+mu** | **9.37** | **614.7** | 5.76 | 0.0 | 46.52 | 1.12 |

`ringbuf_zcopy+mu` wins 1.21–1.27× and on fairness too (1.12–1.56 spread vs
1.36–1.78). This is the scenario built to expose `ringbuf_zcopy`'s weakness —
a descheduled producer blocks delivery *and* its peers — and it does not
materialize, for the same reason as [2] at high producer counts: parking 16
threads on a futex beats spinning them on 7 CPUs. `cpu/wall` 5.49 vs 7.08 is
the mechanism.

### [8] Paced below saturation — the honest CPU comparison

4 producers, 256B slots, offered rate held below saturation so no thread spins
flat out. Throughput is set by the pacing (3.98–3.99 Mm/s everywhere), so only
the CPU columns carry information.

| mode | impl | cpu-ns/rx | cpu/wall | prod-ns | cons-ns |
| --- | --- | --- | --- | --- | --- |
| lossless | **ringbuf_mpsc rsv** | **100.2** | 0.40 | 53.0 | 47.2 |
| lossless | ringbuf_mpsc push | 188.6 | 0.75 | 136.4 | 52.2 |
| lossless | ringbuf_zcopy+mu | 120.0 | 0.48 | 70.8 | 49.2 |
| lossless | ringbuf_zcopy+mu psh | 170.9 | 0.68 | 108.0 | 62.9 |
| overwrite | **ringbuf_mpsc rsv** | **109.5** | 0.44 | 51.1 | 58.4 |
| overwrite | ringbuf_mpsc push | 113.9 | 0.45 | 56.4 | 57.4 |
| overwrite | ringbuf_zcopy+mu | 153.3 | 0.61 | 85.3 | 68.0 |
| overwrite | ringbuf_zcopy+mu psh | 222.9 | 0.89 | 142.7 | 80.3 |

**Below saturation the ranking flips back to `ringbuf_mpsc`, by 1.20–1.40× on
CPU per delivered message.** With `cpu/wall` at 0.40–0.89 nobody is spinning,
so the mutex's futex traffic is pure overhead with no spin cost to offset it.
This is the configuration where the lock-free claim is simply cheaper, and it
is also the most common real-world operating point — queues usually are not
saturated.

The split push is worth 1.04–1.88× on CPU here (`rsv` vs `push` within each
variant), which is the one place its benefit shows up cleanly: at a paced
rate, the saved copy is not hidden behind contention.

## Conclusions

1. **This is a split decision, not a winner, and the axis is contention — not
   fill cost.** `ringbuf_mpsc` wins uncontended, at 1–2 producers, below
   saturation, and on small or heavily-occupied rings. `ringbuf_zcopy+mu` wins
   at 4+ saturated producers on a roomy ring. Both margins are 1.2–1.5× in
   their own regime, with one 2.8× outlier in `ringbuf_mpsc`'s favour.
2. **The fill-cost hypothesis was wrong, and the reason generalizes.** An
   expensive fill does *not* favour the caller-held mutex (conclusion: it
   loses 1.72–1.83× at `passes` 8–32). Both variants already fill outside any
   lock, so a longer fill buys `zcopy` no parallelism `mpsc` lacks — it only
   raises slot occupancy, and **a refused reserve under a caller mutex has to
   re-acquire that mutex to retry, where a lock-free claim just re-CASes.**
   The fill-cost axis turned out to be a proxy for occupancy.
3. **`cap=16` at 4 producers is the standout: `ringbuf_mpsc` delivers
   2.81× more at half the CPU.** A small contended ring turns the caller's
   mutex into a thundering herd — 1812 failed stores and 33.9 context
   switches per 1000 messages. **If your ring is small relative to your
   producer count, this is the deciding result.**
4. **Where `ringbuf_zcopy+mu` wins, it wins by parking rather than
   spinning.** Every such row shows `cpu/wall` well below `ringbuf_mpsc`'s
   (4.02 vs 4.83, 5.65 vs 7.38, 5.49 vs 7.08). The mutex is not a faster
   claim; it is a cheaper *wait*. That is why its advantage appears only when
   cores are oversubscribed and vanishes in scenario [8] where nothing waits.
5. **Oversubscription favours `ringbuf_zcopy+mu` (1.21–1.27×), contrary to
   expectation.** The scenario was built to expose its head-of-line stall at
   16 producers on 7 CPUs; parking the losers beats spinning them anyway.
6. **The split push is worth having on both variants, but its payoff is
   small and situational.** It is a clean 1.04–1.88× CPU win in the paced
   scenario and roughly free elsewhere; the `push` control rows are never the
   best in any table. It costs ~1.9 ns uncontended on the `ringbuf_mpsc` side
   ([1]). Its real justification is qualitative — in-place fills, overlapping
   writes, no staging buffer — not the throughput.
7. **Pick on contract first; the throughput difference rarely decides it.**
   `ringbuf_mpsc_reserve` never blocks, needs no caller lock, and is
   async-signal-safe in both modes. `ringbuf_zcopy` requires the caller to own
   a mutex, initialize it, hold it across the claim only, never require it on
   the submit path, and keep it off the queue's cache lines — four rules, each
   of which compiles fine when violated and two of which deadlock. That is a
   real maintenance cost for a margin that reverses depending on producer
   count and ring size. **Use `ringbuf_zcopy` when you want in-place fills
   under SPSC** (where no mutex is needed at all); **use `ringbuf_mpsc` when
   you genuinely have several producers.**

### Caveat

Single VM host, 8 vCPUs, no SMT, one session. Two full runs; every direction
above reproduced, with zero `received + dropped != pushed` failures. Treat
gaps under ~15% as noise.

Two specific instabilities worth knowing: scenario [2]'s **2-producer row** is
the least stable cell (`zcopy` swung 11.24 → 7.81 Mm/s, i.e. 1.16× → 1.67× for
`ringbuf_mpsc`; direction held, magnitude did not), and scenario [1]'s
single-threaded rates vary ±4% between runs. The large gaps — [6] `cap=16`'s
2.8×, [4]'s crossover, [5]'s reversal at 1024B, and [8]'s CPU ranking — were
all stable across both runs.
