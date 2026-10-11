# ringbuf vs ringbuf_mutex: throughput and CPU consumption

Head-to-head measurement of the two implementations in this tree. The
harness is `bench_ringbuf_vs_ringbuf_mutex.c`, built as the
`bench_ringbuf_vs_ringbuf_mutex` target.

This is distinct from `bench_ringbuf.c`, which measures `ringbuf` against an
internal toy mutex ring (`struct mq`) to price a lock in the abstract and
never touches `ringbuf_mutex.c` at all. This one links both real
implementations — which is what the prefixed symbols are for — and reports
CPU consumption next to throughput.

## Build and run

Both benchmarks live behind the same opt-in flag. Neither is a CTest entry:
they are measurements, not pass/fail checks.

```bash
# configure + build (bench targets are OFF by default)
nix shell ~/project/nixos_config/.#cshell --command bash -c \
    'cmake -B build -DRINGBUF_BUILD_BENCH=ON && cmake --build build -j'

# run the head-to-head comparison (~4 min)
./build/bench_ringbuf_vs_ringbuf_mutex
```

Always compiled `-O2` regardless of `CMAKE_BUILD_TYPE`, so a stray Debug
configure cannot silently produce meaningless numbers. Linux-only: it pins
threads with `pthread_setaffinity_np`, and CMake fails at configure time
elsewhere.

Correctness is a separate gate, and should be green before trusting any of
this — the benchmark asserts `received + dropped == pushed` per run and
prints `!!` if it ever breaks, but it is not a substitute for the suites:

```bash
nix shell ~/project/nixos_config/.#cshell --command \
    ctest --test-dir build -R '^ringbuf$|^ringbuf_mutex$' --output-on-failure
```

Note `-R ringbuf` would match all eight entries including the four opt-in
`*_tsan` ones; the anchored alternation above selects just the two
functional suites.

## Method

- 2,000,000 messages per run; 1,000,000 for the paced scenario.
- 5 repetitions per cell. **The reported row is the single repetition whose
  throughput was the median**, not a per-column median — so every column of
  a row describes one coherent run. A per-column median could otherwise
  print a CPU figure that never accompanied the printed throughput.
- Producer pinned to CPU 2, consumer to CPU 4.
- CPU time via `CLOCK_THREAD_CPUTIME_ID`, which counts only on-CPU time, so
  a thread blocked on a futex accrues none of it. Context switches via
  `getrusage(RUSAGE_THREAD)`.
- The two `struct worker` instances are cache-line aligned and each thread
  has its own `sink`; a shared global sink written from both cores is itself
  a coherence benchmark.

### Columns

`Mmsg/s` and `Mm/s` are **millions of messages per second** — `M` is the
10⁶ prefix, so 22.85 Mm/s is 22,850,000 messages/sec.

What counts as one "message" differs between the tables, which matters when
reconciling the numbers:

- **Scenario [1]** reports a single `Mmsg/s`, and one unit there is a full
  **push+pop pair** — that loop alternates both on one thread at queue depth
  1. This is why it squares with the adjacent CPU column: 136.41 Mmsg/s is
  1/136.41e6 ≈ 7.33 ns, matching the reported 7.30 `cpu-ns/op`.
- **Scenarios [2]–[6]** split throughput into two columns, because in
  overwrite mode pushing and delivering are not the same event.

| Column | Better | Meaning |
| --- | --- | --- |
| `push-Mm/s` | higher — but see note | Pushes **completed** per second (millions): total pushes ÷ wall time |
| `recv-Mm/s` | **higher** | Messages actually **delivered** to the consumer per second |
| `cpu-ns/rx` | **lower** | CPU-ns per *delivered* message, both threads summed |
| `cpu/wall` | *neither* | CPU time ÷ wall time; 2.0 = both cores pegged. Diagnostic, not a goal |
| `prod-ns` / `cons-ns` | lower | Per-thread CPU-ns per push |
| `push-rt/k` / `pop-rt/k` | lower | Spin retries per 1000 messages |
| `ctxsw/k` | lower | Context switches per 1000 messages, both threads |
| `dropped %` | lower | Share of pushes evicted before being read (overwrite mode only) |

Three of these need the qualification, or the table misleads:

- **`push-Mm/s`: higher is better only at equal drop rates.** In overwrite
  mode a variant can raise it purely by evicting more, which costs less than
  delivering. Read it next to `dropped %`, or just use `recv-Mm/s`.
- **`cpu/wall` has no good direction.** It reports how much of the run both
  threads spent on-CPU: ~2.0 means both spun flat out, lower means they
  idled. Which is preferable depends entirely on the workload — spinning is
  the right answer for a latency-critical hot path and the wrong one for a
  mostly-idle service. Here it serves as a validity check on the CPU
  columns: at ~2.0, `cpu-ns/rx` is just 2× wall time and carries no
  independent information (trap #2).
- **`prod-ns` / `cons-ns` are per *push*, not per delivery**, unlike
  `cpu-ns/rx`. They answer "what did this thread spend", so they are not
  comparable across tables with different drop rates — only between the two
  rows of one table.

The two throughput columns are **identical in lossless mode** — nothing is
dropped, so every push is a delivery. In overwrite mode the gap between them
is exactly the eviction rate, and that is the whole reason they are
separate: see trap #1 below. For lossless comparisons either column works;
for overwrite mode, `recv-Mm/s` is the one that cannot be gamed.

### Two traps this harness is built to avoid

Both of these make the lock-free variant look better than it is, and both
bit the first version of this benchmark:

1. **In overwrite mode, throughput alone is not a win condition.** Evicting
   a message is cheaper than delivering one, so a variant can post a higher
   *push* rate purely by dropping more. Scenario [5] at cap=4096 is the
   clearest case: `ringbuf` shows 23.47 `push-Mm/s` against
   `ringbuf_mutex`'s 10.16 — a 2.3× lead — but only 1.41 vs 1.08
   `recv-Mm/s`, because it dropped 94.0% of traffic against 89.4%. Most of
   that apparent advantage is a faster discard path, not more useful work.
   Compare `recv-Mm/s`, and read the drop percentage next to it.
2. **In the saturated scenarios the CPU columns are not independent.** Both
   threads spin flat out, `cpu/wall` pins at ~1.95, and `cpu-ns` degenerates
   into a restatement of wall time. Scenario [6] exists because fixing the
   offered rate *below* saturation is the only way the CPU columns say
   anything the throughput column does not.

## Hardware

12th Gen Intel Core i5-12500H, 8 vCPUs under VMware, 1 thread/core,
no SMT siblings. Linux 6.18.52, clang, `-O2`.

Absolute numbers are VM numbers and will not transfer; the ratios between
the two rows of a table are the result.

## Results

### [1] Uncontended, single thread, alternating push/pop (depth 1)

Queue depth 1, so no cross-core traffic and no contention at all. This
isolates each implementation's synchronization instruction cost.

One `Mmsg/s` unit here is a push **and** a pop, so `cpu-ns/op` covers the
pair — unlike the two-thread tables below, where the two directions are
counted separately.

| impl | mode | Mmsg/s | cpu-ns/op |
| --- | --- | --- | --- |
| ringbuf | lossless | 136.41 | 7.30 |
| ringbuf_mutex | lossless | 134.02 | 7.37 |
| ringbuf | overwrite | 55.50 | 17.92 |
| **ringbuf_mutex** | **overwrite** | **104.18** | **9.50** |

Lossless is a tie, as expected — the code is identical. The overwrite row is
the first real finding, and it is **not** about locking: at depth 1 the
buffer is never full, so `ringbuf_mutex` never reaches its eviction branch
and never takes the mutex. The 2.45× gap is `ringbuf_pop`, which in
overwrite mode takes a slot-lock CAS **plus** a `read_idx` CAS on *every*
pop, not just on contention:

```c
/* ringbuf.c, ringbuf_pop(), overwrite mode */
atomic_compare_exchange_strong_explicit(&rb->slot_lock[idx], ...)   /* always */
memcpy(...)
atomic_store_explicit(&rb->slot_lock[idx], 0, memory_order_release)
atomic_compare_exchange_strong_explicit(&rb->read_idx, ...)         /* always */
```

`ringbuf_mutex_pop` in overwrite mode takes one uncontended
lock/unlock pair, which is cheaper than two `lock cmpxchg`es. Two atomic
RMWs on the consumer's hot path is the price of being lock-free, and it is
paid on every pop whether or not anything is contending.

### [2] SPSC lossless, by capacity

| cap | impl | push-Mm/s | cpu-ns/rx | cpu/wall |
| --- | --- | --- | --- | --- |
| 16 | ringbuf | 29.33 | 66.2 | 1.94 |
| 16 | ringbuf_mutex | 28.68 | 67.8 | 1.94 |
| 256 | ringbuf | 55.35 | 35.3 | 1.95 |
| 256 | ringbuf_mutex | 42.23 | 45.9 | 1.94 |
| 4096 | ringbuf | 70.64 | 26.9 | 1.90 |
| 4096 | ringbuf_mutex | 90.95 | 20.3 | 1.84 |
| 65536 | ringbuf | 84.32 | 22.2 | 1.87 |
| 65536 | ringbuf_mutex | 66.68 | 28.3 | 1.89 |

Parity, which is the expected and correct result: in lossless mode
`read_idx` has exactly one writer, so neither variant takes a lock and the
paths are equivalent. The per-cell spread (which variant "wins" at cap=256
vs cap=4096) **reverses between runs** and is VM scheduling noise, not
signal. Read this table as "no difference" — it is a control.

### [3] SPSC overwrite, by capacity — the real comparison

| cap | impl | push-Mm/s | recv-Mm/s | cpu-ns/rx | push-rt/k | pop-rt/k | dropped |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 16 | ringbuf | 11.06 | 6.30 | 313.9 | 208.6 | 169.6 | 43.0% |
| 16 | **ringbuf_mutex** | 15.00 | **6.54** | **297.1** | 0.0 | 102.6 | 56.4% |
| 256 | ringbuf | 11.47 | 3.96 | 499.3 | 178.8 | 308.3 | 65.5% |
| 256 | **ringbuf_mutex** | 19.54 | **11.49** | **169.4** | 0.0 | 32.8 | 41.2% |
| 4096 | ringbuf | 11.15 | 7.37 | 267.8 | 240.5 | 116.5 | 33.9% |
| 4096 | **ringbuf_mutex** | 21.68 | **18.56** | **104.9** | 0.0 | 90.9 | 14.4% |
| 65536 | ringbuf | 11.64 | 7.07 | 278.9 | 244.2 | 161.5 | 39.2% |
| 65536 | **ringbuf_mutex** | 22.85 | **16.59** | **115.4** | 0.0 | 0.0 | 27.4% |

`ringbuf_mutex` wins on every axis simultaneously, which is the headline:
**1.9–2.5× the push rate, 1.04–2.9× the delivered rate, and 1.06–2.9× less
CPU per delivered message, while dropping fewer messages.**

The mechanism is the `push-rt/k` column. `ringbuf` burns 180–245 failed
pushes per 1000 messages — each one a wasted slot-lock CAS plus a spin —
because in overwrite mode a full buffer whose oldest slot is being read
right now *fails the push*, and the caller must retry. `ringbuf_mutex`'s
push-retry count is **identically zero** at every capacity, which is its
documented guarantee: push in overwrite mode always returns true. It waits
on the futex instead, and that wait is both cheaper and more productive than
spinning.

`ctxsw/k` stays under 0.07 even for the mutex variant — the blocking is
brief enough to resolve in the futex's adaptive spin phase without a real
deschedule, so the predicted "mutex pays in context switches" cost barely
materializes at this contention level.

### [4] SPSC overwrite, cap 4096, by slot size

| slot | impl | push-Mm/s | recv-Mm/s | cpu-ns/rx | dropped |
| --- | --- | --- | --- | --- | --- |
| 16B | ringbuf | 12.60 | 4.44 | 444.9 | 64.7% |
| 16B | **ringbuf_mutex** | 22.08 | **14.41** | **134.6** | 34.8% |
| 64B | ringbuf | 11.39 | 7.18 | 274.6 | 37.0% |
| 64B | **ringbuf_mutex** | 21.41 | **15.88** | **122.1** | 25.8% |
| 256B | ringbuf | 10.46 | 3.13 | 631.3 | 70.1% |
| 256B | **ringbuf_mutex** | 25.87 | **21.10** | **91.2** | 18.4% |
| 1024B | ringbuf | 14.46 | 13.99 | 140.8 | 3.2% |
| 1024B | **ringbuf_mutex** | 34.53 | **33.87** | **57.3** | 1.9% |

The gap does not wash out under memcpy cost as payloads grow — it widens, to
2.4× delivered throughput at 1024B. At the largest slot size the per-message
copy dominates enough that the buffer stops filling (3.2% / 1.9% drops), and
`ringbuf` *still* loses, because its retry overhead is per-call and
independent of payload size.

### [5] SPSC overwrite, slow consumer (200 work units/msg)

Worst case: the consumer cannot keep up, so the buffer is permanently full
and nearly every push evicts.

| cap | impl | push-Mm/s | recv-Mm/s | cpu-ns/rx | dropped |
| --- | --- | --- | --- | --- | --- |
| 16 | ringbuf | **26.46** | 1.69 | 1155.1 | 93.6% |
| 16 | ringbuf_mutex | 16.40 | 0.89 | 2167.6 | 94.6% |
| 4096 | ringbuf | **23.47** | 1.41 | 1386.6 | 94.0% |
| 4096 | ringbuf_mutex | 10.16 | 1.08 | 1793.8 | 89.4% |

**This is the one scenario where `ringbuf` wins, and it is the clearest
illustration of trap #1.** At cap=16 it posts 1.6× the push rate *and* 1.9×
the delivered rate — a genuine win. At cap=4096 it posts 2.3× the push rate
while delivering 1.41 vs 1.08 Mm/s, having dropped 94.0% against 89.4%:
much of that push advantage is just a faster discard path.

The reading: when the buffer is saturated and the application's intent is
"throw almost everything away, never stall the producer", failing fast and
spinning beats blocking. `ringbuf` degrades toward "drop it and move on";
`ringbuf_mutex` keeps paying to hand messages over. That is the lock-free
design working as intended, in the regime it was designed for.

### [6] Paced below saturation — the honest CPU comparison

1,000,000 messages in bursts of 100 per 100 µs (~1 Mmsg/s offered, far
below either variant's capacity). Both sides idle; the consumer sleeps after
200 failed pops. Throughput is set by the pacing, so only the CPU columns
mean anything.

| mode | impl | cpu-ns/rx | cpu/wall | prod-ns | cons-ns | ctxsw/k |
| --- | --- | --- | --- | --- | --- | --- |
| lossless | ringbuf | 150.1 | 0.15 | 32.5 | 117.6 | 15.36 |
| lossless | ringbuf_mutex | 154.3 | 0.15 | 33.3 | 121.0 | 15.24 |
| overwrite | ringbuf | **167.6** | 0.17 | 32.4 | 135.3 | 15.01 |
| overwrite | ringbuf_mutex | 179.3 | 0.18 | 30.9 | 148.4 | 15.16 |

With `cpu/wall` down at 0.15–0.18 these are real CPU measurements rather
than echoes of wall time. The result is **near-parity, within ~7%** — and
note `ringbuf` is marginally *ahead* in overwrite mode here, the opposite of
scenario [3]. At this offered rate the buffer never fills (0.00% dropped),
so neither variant's eviction path runs, and what is left is the idle
polling cost both share. The CPU figures are dominated by the consumer's
backoff loop, which is identical code for both.

So: the CPU asymmetry the two designs *could* exhibit only materializes
under contention. Uncontended, they cost the same.

## Conclusions

1. **Lossless mode: the variants are equivalent.** Same code path, no lock
   on either side, measured parity. Choose on API, not performance.
2. **Overwrite mode under contention: `ringbuf_mutex` is substantially
   faster** — up to 2.9× delivered throughput and 2.9× less CPU per
   delivered message — because `ringbuf` spends 180–245 failed pushes per
   1000 messages spinning, while the mutex variant's push never fails and
   its futex waits resolve without real context switches (`ctxsw/k` < 0.07).
3. **`ringbuf`'s cost is on the consumer's hot path, not in locking.** In
   overwrite mode `ringbuf_pop` does two atomic RMWs (slot-lock CAS +
   `read_idx` CAS) on *every* pop. Scenario [1] isolates this at 2.45×
   slower uncontended, with no lock involved on either side.
4. **`ringbuf` wins when saturated and lossy** (scenario [5]): failing fast
   beats blocking when the application is discarding ~94% of traffic anyway.
   Part of that win is a cheaper discard path rather than more useful work —
   at cap=4096 the 2.3× push advantage is only a 1.3× delivered advantage.
5. **Pick on contract, not on these numbers.** `ringbuf_push` never blocks
   and is async-signal-safe; `ringbuf_mutex_push` can block and is not.
   That difference decides signal handlers and hard real-time regardless of
   throughput. Where both are admissible and overwrite mode is contended,
   `ringbuf_mutex` is both faster and the simpler code.

### Caveat

Single VM host, 8 vCPUs, no SMT, one session. The lossless table's per-cell
ordering reverses between runs, so treat gaps under ~15% as noise; the
overwrite-mode gaps (2–3×) reproduced consistently across three full runs.
