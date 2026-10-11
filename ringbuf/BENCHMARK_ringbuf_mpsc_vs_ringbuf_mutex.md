# ringbuf_mpsc vs ringbuf_mutex: multi-producer fan-in

Head-to-head measurement of `ringbuf_mpsc` against `ringbuf_mutex` made
usable from N producers by a caller-held push mutex. The harness is
`bench_ringbuf_mpsc_vs_ringbuf_mutex.c`, built as the
`bench_ringbuf_mpsc_vs_ringbuf_mutex` target.

**The focus is `ringbuf_mpsc`.** `ringbuf_mutex` is here as the baseline a
caller would otherwise have to build, not as a peer implementation.

## Why this comparison needs a wrapper at all

`BENCHMARK_ringbuf_vs_ringbuf_mutex.md` compares two variants that accept
the *same* SPSC contract, so the only difference is how overwrite mode is
synchronized and a direct call-for-call comparison is fair. **These two do
not share a contract.** `ringbuf_mpsc_push` natively accepts any number of
concurrent producers; `ringbuf_mutex_push` permits exactly one, and calling
it from two threads at once is undefined behavior, not a slow result.

So the baseline is what a caller would actually write if `ringbuf_mutex`
were all they had — serialize the producers themselves:

```c
pthread_mutex_lock(&push_lock);
ok = ringbuf_mutex_push(rb, msg);
pthread_mutex_unlock(&push_lock);
```

This row is labelled **`ringbuf_mutex+mu`** throughout.

### Why that wrapper is legal

Worth stating, because it is the premise of the whole file. The SPSC
contract forbids *concurrent* pushers, not a rotating producer identity.
Every piece of producer-private state inside `ringbuf_mutex` — `cached_read`,
and the release store to `write_idx` — is handed from one producer thread to
the next through `push_lock`'s own acquire/release edges, so one-at-a-time
from many threads is exactly as safe as always-the-same-thread.

The consumer never takes `push_lock`. Only producers contend for it, which
is precisely the cost being priced.

### Two deliberate choices in the wrapper

- **The retry loop lives outside `push_lock`.** Spinning on a full buffer
  while holding it would block every other producer behind a condition only
  the consumer can clear — a bug in the wrapper, not a property of
  `ringbuf_mutex`.
- **`push_lock` sits in its own cache line** (`_Alignas(64)`), so the
  comparison is not measuring false sharing between the lock and the queue
  pointers that every producer reads on every push.

## Build and run

```bash
nix shell ~/project/nixos_config/.#cshell --command bash -c \
    'cmake -B build -DRINGBUF_BUILD_BENCH=ON && cmake --build build -j'

./build/bench_ringbuf_mpsc_vs_ringbuf_mutex    # ~4.5 min
```

Opt-in behind the same flag as the other two benchmarks, and deliberately
not a CTest entry: these are measurements, not pass/fail checks. Always
compiled `-O2` regardless of `CMAKE_BUILD_TYPE`. Linux-only — it pins
threads with `pthread_setaffinity_np`.

Correctness is a separate gate and should be green before trusting any of
this. The benchmark asserts `received + dropped == pushed` per run and
prints `!!` if it ever breaks, but that is not a substitute for the suites:

```bash
nix shell ~/project/nixos_config/.#cshell --command ctest --test-dir build \
    -R '^ringbuf_mpsc$|^ringbuf_mutex$|^ringbuf_mpsc_tsan$|^ringbuf_mutex_tsan$' \
    --output-on-failure
```

## Method

- 2,000,000 messages per run, **split across the producers** (1,000,000 for
  the paced scenario), so wall throughput stays comparable as the producer
  count changes. The remainder goes to the first few producers rather than
  being dropped.
- 5 repetitions per cell. **The reported row is the single repetition whose
  throughput was the median**, not a per-column median — so every column of
  a row describes one coherent run.
- Consumer pinned to CPU 0, producers to CPUs 1..7, cycling once they run
  out. Cycling rather than refusing is what makes the oversubscribed
  scenario expressible on any host.
- CPU time via `CLOCK_THREAD_CPUTIME_ID`, which counts only on-CPU time, so
  a thread blocked on a futex accrues none of it. Context switches via
  `getrusage(RUSAGE_THREAD)`.
- Each `struct worker` is cache-line aligned with its own `sink`; a shared
  global sink written from several cores is itself a coherence benchmark.

### Two differences from the SPSC benchmark — the numbers are NOT comparable

1. **`cpu/wall` saturates near `producers+1`, not 2.0.** A row with 7
   producers pegging every core reads ~7.1, not ~1.95.
2. **A failed push or pop yields after 64 consecutive failures** instead of
   spinning on `cpu_relax()` forever. This is load-bearing for scenario [6]:
   with more producers than cores, `ringbuf_mpsc` can have a producer
   holding a ticket it has not published yet, which blocks delivery
   head-of-line until that thread is scheduled again. A pure spin there is
   not a measurement. The yield is what a real caller would do, it is
   applied identically to both variants, and on a thread pinned to its own
   idle CPU it costs essentially nothing — so it does not distort the
   undersubscribed rows.

### Columns

Same as the SPSC benchmark, with one addition. `Mm/s` is **millions of
messages per second**.

| Column | Better | Meaning |
| --- | --- | --- |
| `push-Mm/s` | higher — at equal drop rates | Pushes **completed** per second: total pushes ÷ wall time |
| `recv-Mm/s` | **higher** | Messages actually **delivered** to the consumer per second |
| `cpu-ns/rx` | **lower** | CPU-ns per *delivered* message, all threads summed |
| `cpu/wall` | *neither* | CPU ÷ wall; `producers+1` = every core pegged. Diagnostic |
| `prod-ns` / `cons-ns` | lower | CPU-ns per push; `prod-ns` sums **all** producers |
| `push-rt/k` / `pop-rt/k` | lower | Spin retries per 1000 messages |
| `ctxsw/k` | lower | Context switches per 1000 messages, every thread |
| `dropped %` | lower | Share of pushes evicted before being read |
| **`finish spread`** | **lower, 1.00 ideal** | Slowest producer's wall time ÷ fastest's |

**`finish spread` is the fairness column, and it is new here.** With N
producers sharing one queue, how evenly the N shares complete is a property
of the synchronization: a mechanism that lets one producer barge repeatedly
shows a spread well above 1.0. It means nothing with a single producer.

The caveats from the SPSC benchmark carry over unchanged: `push-Mm/s` is
only a win condition at equal drop rates, `cpu/wall` has no good direction
and serves as a validity check on the CPU columns, and `prod-ns`/`cons-ns`
are per *push* so they are comparable only between the two rows of one
table.

## Hardware

12th Gen Intel Core i5-12500H, 8 vCPUs under VMware, 1 thread/core, no SMT
siblings. Linux 6.18.52, clang, `-O2`.

Absolute numbers are VM numbers and will not transfer; the ratios between
the two rows of a table are the result. Everything below reproduced across
two full runs, with zero accounting failures in either.

## Results

### [1] Uncontended, single thread, alternating push/pop (depth 1)

Queue depth 1, so no cross-core traffic and no contention at all. The
`ringbuf_mutex raw` row takes **no push lock** — legal only because there is
one thread here — so the gap from it to `+mu` prices the external mutex on
its own, separately from any contention for it.

One `Mmsg/s` unit is a push **and** a pop, so `cpu-ns/op` covers the pair.

| impl | mode | Mmsg/s | cpu-ns/op |
| --- | --- | --- | --- |
| ringbuf_mpsc | lossless | 73.24 | 13.42 |
| ringbuf_mutex+mu | lossless | 104.08 | 9.57 |
| **ringbuf_mutex raw** | **lossless** | **137.54** | **7.25** |
| ringbuf_mpsc | overwrite | 41.13 | 24.19 |
| ringbuf_mutex+mu | overwrite | 75.89 | 13.13 |
| **ringbuf_mutex raw** | **overwrite** | **103.11** | **9.66** |

Two numbers fall straight out of this, and both are instruction cost rather
than synchronization:

- **The external mutex costs ~2.3 ns lossless, ~3.5 ns overwrite**
  (`raw` → `+mu`), uncontended. That is one `pthread_mutex_lock`/`unlock`
  pair on its fast path.
- **`ringbuf_mpsc` costs ~6.2 ns more than `raw` lossless, and ~14.5 ns more
  in overwrite mode.** This is the price of the ticket design, paid on every
  call whether or not anything contends. Lossless, `push_lossless` adds one
  `lock cmpxchg` (the ticket CAS) that `ringbuf_mutex_push` does not have.
  Overwrite mode is where it gets expensive — `pop_overwrite` does **three**
  atomic RMWs on every pop:

```c
/* ringbuf_mpsc.c, pop_overwrite() — all three run on every pop */
atomic_compare_exchange_strong_explicit(&rb->slot_lock[idx], ...)   /* 1 */
memcpy(...)
atomic_compare_exchange_strong_explicit(&rb->stamp[idx], ...)       /* 2 */
advance_read_idx(rb, r + 1);                                        /* 3 */
```

plus the ticket CAS on the push side: four atomic RMWs per push+pop pair.
`ringbuf_mutex raw` in overwrite mode pays one uncontended mutex
lock/unlock in `pop` and nothing at all on `push`'s fast path, which is
cheaper than four `lock cmpxchg`es.

This mirrors conclusion 3 of the SPSC benchmark — `ringbuf`'s overwrite cost
is also on the consumer's hot path rather than in locking — and it is the
same shape of finding in a different variant. **`ringbuf_mpsc` enters every
contended scenario below carrying this handicap.**

### [2] Lossless, cap 4096, by producer count — the headline

| producers | impl | recv-Mm/s | cpu-ns/rx | cpu/wall | push-rt/k | pop-rt/k | ctxsw/k | spread |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | **ringbuf_mpsc** | **26.39** | **74.4** | 1.96 | 5.3 | 95.4 | 0.01 | — |
| 1 | ringbuf_mutex+mu | 19.02 | 103.2 | 1.96 | 0.2 | 367.0 | 0.01 | — |
| 2 | **ringbuf_mpsc** | **14.64** | **194.7** | 2.85 | 3.4 | 427.5 | 0.01 | 1.14 |
| 2 | ringbuf_mutex+mu | 5.74 | 513.4 | 2.95 | 1.2 | 2747.2 | 0.13 | **1.01** |
| 4 | **ringbuf_mpsc** | **10.86** | **425.6** | 4.62 | 2.5 | 559.7 | 0.02 | 1.33 |
| 4 | ringbuf_mutex+mu | 6.15 | 648.7 | 3.99 | 0.1 | 2092.3 | 11.43 | **1.06** |
| 7 | **ringbuf_mpsc** | **7.65** | **927.2** | 7.09 | 14.5 | 873.4 | 0.11 | 1.55 |
| 7 | ringbuf_mutex+mu | 4.87 | 1159.1 | 5.65 | 19.1 | 3139.0 | 44.13 | **1.04** |

**`ringbuf_mpsc` wins at every producer count — 1.4× to 2.6× delivered
throughput, and 1.25× to 2.6× less CPU per delivered message.** The margin
peaks at 2 producers (2.55×) and narrows as the count rises, because by 7
producers both variants are limited by the single consumer rather than by
their own fan-in.

Note the single-producer row: `ringbuf_mpsc` wins there too, 1.39×, despite
scenario [1] showing it is *more* expensive per call uncontended. The
difference is that scenario [1] measures one thread alternating; here the
producer and consumer run on separate cores, and `+mu`'s extra lock/unlock
pair sits on the critical path between them.

Two columns explain the mechanism:

- **`pop-rt/k` is 3–6× higher for `+mu`.** The consumer is being *starved*.
  While producers serialize on `push_lock`, `write_idx` advances in
  single file, so the consumer repeatedly finds the buffer empty and spins.
  `ringbuf_mpsc`'s producers claim tickets concurrently, so messages arrive
  in parallel and the consumer finds work.
- **`ctxsw/k` reaches 44 for `+mu` against 0.11 for `ringbuf_mpsc`.** This
  is the predicted cost of blocking, and unlike the SPSC benchmark (where it
  stayed under 0.07 and barely materialized) it is real here: with 7
  producers the futex's adaptive spin phase is not enough, and losers get
  genuinely descheduled.

**`finish spread` goes the other way, and it is the mutex's one clear win.**
`+mu` holds 1.01–1.06 at every count while `ringbuf_mpsc` drifts to 1.55 at
7 producers. A mutex serializes producers into something close to a queue;
the ticket CAS lets a producer whose cache line is already hot win
repeatedly. If per-producer latency fairness matters more than aggregate
throughput, that is a genuine trade and this is the column to read.

### [3] Overwrite, cap 4096, by producer count

| producers | impl | push-Mm/s | recv-Mm/s | cpu-ns/rx | push-rt/k | dropped | spread |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | ringbuf_mpsc | 18.39 | 18.25 | 108.5 | 213.7 | 0.76% | — |
| 1 | **ringbuf_mutex+mu** | **25.65** | **25.64** | **77.0** | **0.0** | 0.03% | — |
| 2 | **ringbuf_mpsc** | 14.03 | **13.85** | **192.1** | 78.5 | 1.30% | 1.46 |
| 2 | ringbuf_mutex+mu | 7.27 | 7.26 | 404.1 | **0.0** | 0.14% | **1.03** |
| 4 | **ringbuf_mpsc** | 11.15 | **11.09** | **398.4** | 22.1 | 0.57% | 1.44 |
| 4 | ringbuf_mutex+mu | 6.22 | 6.21 | 654.4 | **0.0** | 0.17% | **1.05** |
| 7 | **ringbuf_mpsc** | 7.46 | **7.44** | **988.6** | 78.9 | 0.28% | 1.34 |
| 7 | ringbuf_mutex+mu | 5.02 | 5.02 | 1113.4 | **0.0** | 0.00% | **1.12** |

**The single-producer row flips, and it is the most informative cell in the
file.** With one producer `ringbuf_mutex+mu` wins 1.40× on delivered
throughput — because there is no fan-in to parallelize, so what is left is
exactly the per-call cost scenario [1] isolated. `ringbuf_mpsc`'s
`push-rt/k` of 213.7 is the visible half of it: a push that needs to evict
takes the slot lock *before* the ticket CAS, and loses it whenever the
consumer is mid-read of that slot.

**From two producers on, `ringbuf_mpsc` wins again, 1.5–1.9×.** The fan-in
parallelism more than pays for the handicap. `+mu`'s `push-rt/k` is
**identically zero** at every count, which is `ringbuf_mutex`'s documented
guarantee — push in overwrite mode always returns true — and it is doing
exactly what the contract says, just not fast enough to matter here.

Drop rates stay under 1.3% for both, so `push-Mm/s` and `recv-Mm/s` barely
diverge and trap #1 does not bite in this table.

### [4] Overwrite, 4 producers, by capacity

| cap | impl | recv-Mm/s | cpu-ns/rx | push-rt/k | pop-rt/k | dropped |
| --- | --- | --- | --- | --- | --- | --- |
| 16 | **ringbuf_mpsc** | **8.16** | **589.9** | 218.5 | 53.7 | 2.11% |
| 16 | ringbuf_mutex+mu | 4.98 | 803.8 | 0.0 | 1788.0 | 1.26% |
| 256 | **ringbuf_mpsc** | **9.89** | **478.2** | 106.2 | 32.4 | 0.70% |
| 256 | ringbuf_mutex+mu | 6.31 | 633.8 | 0.0 | 1441.0 | 0.46% |
| 4096 | **ringbuf_mpsc** | **10.81** | **404.2** | 7.2 | 33.5 | 0.08% |
| 4096 | ringbuf_mutex+mu | 6.39 | 629.2 | 0.0 | 1431.1 | 0.00% |
| 65536 | **ringbuf_mpsc** | **10.41** | **433.3** | 5.8 | 27.7 | 0.09% |
| 65536 | ringbuf_mutex+mu | 6.47 | 620.9 | 0.0 | 1402.4 | 0.00% |

`ringbuf_mpsc` wins at every capacity, 1.6–1.7×, and the margin is
remarkably flat. Its `push-rt/k` climbs sharply as capacity falls (5.8 →
218.5) exactly as predicted — a small ring fills constantly, so the eviction
path runs constantly and the slot lock is contested — but that cost does not
erase the fan-in advantage even at cap=16.

`+mu`'s `pop-rt/k` stays pinned near 1400–1800 regardless of capacity,
confirming that its bottleneck is producer serialization, not the ring
geometry.

### [5] Lossless, 4 producers, cap 4096, by slot size

| slot | impl | recv-Mm/s | cpu-ns/rx | prod-ns | cons-ns | ctxsw/k |
| --- | --- | --- | --- | --- | --- | --- |
| 16B | **ringbuf_mpsc** | **9.94** | **476.4** | 376.6 | 99.8 | 0.01 |
| 16B | ringbuf_mutex+mu | 5.81 | 692.6 | 521.2 | 171.3 | 11.64 |
| 64B | **ringbuf_mpsc** | **10.05** | **461.1** | 362.4 | 98.7 | 0.01 |
| 64B | ringbuf_mutex+mu | 5.82 | 691.3 | 520.8 | 170.5 | 12.42 |
| 256B | **ringbuf_mpsc** | **10.08** | **476.1** | 378.2 | 97.9 | 0.01 |
| 256B | ringbuf_mutex+mu | 4.85 | 813.4 | 609.4 | 204.0 | 15.05 |
| 1024B | **ringbuf_mpsc** | **12.32** | **376.1** | 295.9 | 80.3 | 0.01 |
| 1024B | ringbuf_mutex+mu | 4.08 | 928.5 | 685.8 | 242.7 | 23.78 |

**The gap does not wash out under memcpy cost as payloads grow — it widens
to 3.02× at 1024B,** the largest margin anywhere in this benchmark. The
reason is structural: `+mu` holds `push_lock` *across the memcpy*, so the
copy itself becomes part of the serialized critical section and a larger
payload directly lengthens it. `ringbuf_mpsc` copies outside any lock —
the ticket CAS reserves the slot, and the memcpy runs concurrently with
every other producer's.

`ctxsw/k` rising 11.6 → 23.8 for `+mu` as the payload grows is the same
effect seen from the other side: a longer critical section means more
waiters that fail to resolve in the futex's spin phase.

### [6] Oversubscribed: 16 producers on 7 producer CPUs

This is the scenario constructed to **favor the mutex**. A descheduled
`ringbuf_mpsc` producer holding an unpublished ticket blocks delivery
head-of-line until it is rescheduled; a descheduled producer waiting on
`push_lock` blocks nothing.

| mode | impl | recv-Mm/s | cpu-ns/rx | cpu/wall | push-rt/k | ctxsw/k | spread |
| --- | --- | --- | --- | --- | --- | --- | --- |
| lossless | **ringbuf_mpsc** | **6.98** | **1002.7** | 7.00 | 787.6 | 12.51 | **1.34** |
| lossless | ringbuf_mutex+mu | 3.75 | 1539.4 | 5.77 | 0.0 | 105.48 | 1.56 |
| overwrite | **ringbuf_mpsc** | **7.12** | **1011.9** | 7.21 | 56.6 | 0.94 | **1.55** |
| overwrite | ringbuf_mutex+mu | 3.80 | 1481.3 | 5.63 | 0.0 | 100.37 | 2.01 |

**The predicted mutex advantage does not materialize.** `ringbuf_mpsc` still
wins 1.86× in both modes. The head-of-line stall is visibly real — its
lossless `push-rt/k` jumps to 787.6, by far the highest in the benchmark,
which is producers spinning and yielding on a ticket someone else has not
published — but it costs less than 16 threads funnelling through one mutex.

`+mu` pays ~105 context switches per 1000 messages in both modes — ~8× the
mpsc figure lossless and ~107× it in overwrite mode — and that is where its
throughput goes.

**The fairness column also inverts.** `+mu`'s `finish spread` degrades to
1.56 / 2.01, *worse* than `ringbuf_mpsc`'s, reversing its clean advantage
from scenarios [2]–[4]. Under oversubscription `pthread_mutex_t`'s barging
behavior stops approximating a fair queue: a producer that gets rescheduled
onto a hot cache line reacquires the lock ahead of threads that have been
waiting longer. The mutex's fairness is a property of light contention, not
of the mutex.

### [7] Slow consumer, 4 producers, overwrite, 200 work units/msg

Worst case for eviction: the consumer cannot keep up, so the buffer is
permanently full and most pushes evict.

| cap | impl | push-Mm/s | recv-Mm/s | cpu-ns/rx | push-rt/k | ctxsw/k | dropped |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 16 | ringbuf_mpsc | **3.69** | 1.04 | 4716.3 | 2378.1 | 0.06 | 71.86% |
| 16 | **ringbuf_mutex+mu** | 3.07 | **1.24** | **2563.0** | 0.0 | 32.48 | 59.83% |
| 4096 | ringbuf_mpsc | **3.92** | 1.09 | 4442.3 | 2204.5 | 0.04 | 72.14% |
| 4096 | **ringbuf_mutex+mu** | 3.45 | **1.87** | **1714.6** | 0.0 | 29.61 | 45.59% |

**This is the one scenario `ringbuf_mpsc` loses, and it is a textbook
instance of trap #1.** It posts the higher *push* rate in both rows — and
delivers 16–42% *less*, while dropping 72% against 46–60%. Most of its
apparent push advantage is a faster discard path, not more useful work. Its
`cpu-ns/rx` is 1.8–2.6× worse: it is burning 2200–2400 failed pushes per
1000 messages to achieve that.

The mechanism is `ringbuf_mpsc`'s eviction path colliding with a consumer
that is almost always mid-read. Four producers all want to evict the one
slot at `read_idx`; the consumer holds its lock for a long time because of
the dummy work; so pushes fail, spin, and retry. `ringbuf_mutex+mu` blocks
on the futex instead, which costs 30 context switches per 1000 messages and
is still the better deal.

**Note this is the inverse of the SPSC benchmark's scenario [5]**, where
`ringbuf` (lock-free) beat `ringbuf_mutex` precisely because failing fast
beat blocking in the saturated-and-lossy regime. The reason it reverses:
there the single lock-free producer's retries cost one core's spin cycles,
whereas here four producers' retries also contend with each other on the
same slot lock, so the spinning is no longer free. **Lock-free "fail fast
and retry" degrades badly once there are several threads failing at once.**

### [8] Paced below saturation — the honest CPU comparison

1,000,000 messages in bursts of 100 per 100 µs per producer, far below
either variant's capacity. All sides idle; the consumer sleeps after 200
failed pops. Throughput is set by the pacing, so only the CPU columns mean
anything.

| mode | impl | cpu-ns/rx | cpu/wall | prod-ns | cons-ns | ctxsw/k |
| --- | --- | --- | --- | --- | --- | --- |
| lossless | **ringbuf_mpsc** | **101.3** | 0.40 | 53.7 | 47.5 | 8.95 |
| lossless | ringbuf_mutex+mu | 106.2 | 0.42 | 61.4 | 44.7 | 9.37 |
| overwrite | **ringbuf_mpsc** | **103.6** | 0.41 | 47.8 | 55.7 | 9.06 |
| overwrite | ringbuf_mutex+mu | 139.0 | 0.55 | 72.5 | 66.4 | 9.16 |

With `cpu/wall` at 0.40–0.55 — against a ceiling of 5.0 for 4 producers plus
a consumer — these are real CPU measurements rather than echoes of wall
time. Lossless is **near-parity within 5%**: at this offered rate neither
variant's producers contend, and what remains is idle polling cost they
share. Overwrite shows a 1.34× `ringbuf_mpsc` advantage, which is larger
than noise across both runs.

So, as in the SPSC benchmark, the CPU asymmetry between the two designs only
materializes under contention — but note that the *sign* here is the
opposite of scenario [7]'s, and uncontended it mostly vanishes.

## Conclusions

1. **For multi-producer fan-in, `ringbuf_mpsc` is substantially faster:
   1.45–3.0× delivered throughput and 1.25–2.6× less CPU per delivered
   message**, in both modes, at every producer count from 2 to 16, at every
   capacity and every slot size tested — **except** when the consumer is the
   bottleneck (conclusion 5). This is the result the variant exists for and
   it holds broadly.
2. **The margin widens with payload size, to 3.0× at 1024B.** The mutex
   wrapper holds `push_lock` across the memcpy, so the copy is inside the
   serialized section; `ringbuf_mpsc` copies concurrently after reserving a
   slot with the ticket CAS. This is the most structural of the advantages.
3. **With exactly one producer in overwrite mode, `ringbuf_mutex+mu` wins
   1.40×** — there is no fan-in to parallelize, so what is left is
   `pop_overwrite`'s three atomic RMWs per pop plus the ticket CAS, isolated
   at ~14.5 ns/op in scenario [1]. **Do not use `ringbuf_mpsc` for a
   genuinely single-producer workload**; that is what the other variants are
   for. This is the same shape of finding as conclusion 3 of the SPSC
   benchmark, in a different variant.
4. **Oversubscription does not flip the result.** At 16 producers on 7 CPUs
   — the configuration built to expose `ringbuf_mpsc`'s head-of-line stall —
   it still wins 1.86×. The stall is real and visible (787 failed pushes per
   1000 messages) but costs less than funnelling 16 threads through one
   mutex at 105 context switches per 1000 messages.
5. **`ringbuf_mpsc` loses when the consumer is the bottleneck and most
   traffic is dropped** (scenario [7]): 16–42% less delivered, 1.8–2.6× more
   CPU, while posting a *higher* push rate purely by discarding more. This
   **inverts** the SPSC benchmark's equivalent scenario, where lock-free
   fail-fast won. The reason is the generalizable one: with several
   producers, retries contend with each other, so "fail fast and spin" stops
   being cheap.
6. **The mutex buys fairness, but only under light contention.** `+mu` holds
   a `finish spread` of 1.01–1.06 against `ringbuf_mpsc`'s 1.1–1.9 in
   scenarios [2]–[4] — a real trade if per-producer latency matters more than
   aggregate throughput. But at 16 producers it degrades to 1.56–2.01, worse
   than `ringbuf_mpsc`, as `pthread_mutex_t`'s barging stops approximating a
   fair queue.
7. **Pick on contract first.** `ringbuf_mpsc_push` never blocks and is
   async-signal-safe in both modes; the mutex wrapper is neither, and it also
   adds a lock the caller has to own, initialize, and keep off the queue's
   cache lines correctly. That decides signal handlers and hard real-time
   regardless of throughput. Where both are admissible and there is more
   than one producer, `ringbuf_mpsc` is both faster and less for the caller
   to get wrong.

### Caveat

Single VM host, 8 vCPUs, no SMT, one session. Two full runs; every direction
above reproduced, with zero `received + dropped != pushed` failures. Treat
gaps under ~15% as noise — scenario [1]'s `ringbuf_mpsc` lossless row
swung 73 → 98 Mmsg/s between runs, and scenario [8]'s lossless rows are
parity. The large gaps (1.5–3×) in scenarios [2]–[7] were stable, as was the
direction of the two reversals in [3] at one producer and in [7].
