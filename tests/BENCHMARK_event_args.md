# Event arguments and the 48-byte record, under pftrace

Cost of the record-size change (32 → 48 bytes per ring slot) and of event
arguments (`pf_slice_begin_args`, up to `PF_MAX_ARGS` = 2). Measured with
`tests/bench_args.c`.

## Commits measured

- **Args tree:** `dd756e3` ("pftrace: add event arguments and grow the record
  to 48 bytes"). 48-byte records, args API.
- **Baseline tree:** `d402de9`, the commit before the args change. 32-byte
  records, no args API.
- **Benchmark source:** `tests/bench_args.c` as of the commit that adds the
  cache-sized sections (`git log -- tests/bench_args.c`). Both trees were
  built with that one file, copied in, so the only difference between the
  two binaries is the library.

## How the benchmark is built

The benchmark is a CMake target, not a test. It is gated by the existing
`PFTRACE_BUILD_BENCH` option, which is off by default. The target is
`bench_args`, defined in `tests/CMakeLists.txt`, next to `bench`.

Toolchain: clang 21.1.8, CMake 4.1.6, Release build. Any recent clang or GCC
with C11 should build the same way.

Args tree (`dd756e3`):

```sh
cmake -S . -B /tmp/pf_args_build -DCMAKE_BUILD_TYPE=Release -DPFTRACE_BUILD_BENCH=ON
cmake --build /tmp/pf_args_build --target bench_args -j8
```

Baseline tree (`d402de9`), out of tree so the working copy is untouched:

```sh
rm -rf /tmp/pf_base && mkdir -p /tmp/pf_base
git archive d402de9 | tar -x -C /tmp/pf_base
cp tests/bench_args.c /tmp/pf_base/tests/
# add the bench_args target to /tmp/pf_base/tests/CMakeLists.txt, next to
# bench (the same two lines as in this tree's tests/CMakeLists.txt)
cmake -S /tmp/pf_base -B /tmp/pf_base/build -DCMAKE_BUILD_TYPE=Release \
      -DPFTRACE_BUILD_BENCH=ON
cmake --build /tmp/pf_base/build --target bench_args -j8
```

The baseline build has no args API, so the args sections in `bench_args.c`
are compiled out (`#ifdef PF_MAX_ARGS`). The same file builds in both trees.

## How the benchmark is run

```sh
/tmp/pf_args_build/bench_args            # args tree
/tmp/pf_base/build/bench_args             # baseline tree
```

Each run prints `name value unit` lines. The snapshot sections write their
trace under the current directory (`bench_args.d/`), so run it from a scratch
directory if the checkout should stay clean. Run it three times and take the
median. The binary takes no arguments.

## Method

- Snapshot mode, protobuf output, 1,000,000 begin/end pairs per section, so
  2,000,000 events.
- The producer loop is timed with a direct `clock_gettime` syscall.
- The library reads the clock once per event. On this machine that read
  costs about 8.5 µs, so the benchmark replaces the library's
  `clock_gettime` with a counter for the library's timestamps only. The
  timing loops are not affected. This isolates the ring and encode path.
- The drain figure is the time for `pf_trace_dump` divided by events.
- Two ring sizes: 256 MiB (every event fits; the whole run is written to
  RAM) and 256 KiB (fits in cache, so the writes stay in cache).

## Results, run set A (large ring, medians of 3 runs)

| Measurement | Baseline (32 B) | Args tree (48 B) |
|---|---|---|
| producer, no args | 68.9 ns/event | 93.5 ns/event |
| producer, 2 args | — | 95.7 ns/event |
| drain (dump), no args | 65.0 ns/event | 68.7 ns/event |
| drain (dump), 2 args | — | 132.7 ns/event |
| bytes on disk, no args | 17.9 B/event | 17.9 B/event |
| bytes on disk, 2 args | — | 30.4 B/event |
| real `clock_gettime` read | 8.5 µs/call | 8.5 µs/call |

Raw runs (ns/event):

| Run | Baseline produce | Baseline dump | New produce (no args) | New produce (2 args) | New dump (no args) | New dump (2 args) |
|---|---|---|---|---|---|---|
| 1 | 68.9 | 63.6 | 97.6 | 95.7 | 67.4 | 134.2 |
| 2 | 77.8 | 65.0 | 93.5 | 92.6 | 69.6 | 131.3 |
| 3 | 67.4 | 102.1 | 91.9 | 102.7 | 68.7 | 132.7 |

## Results, run set B (large vs. cache-sized ring, medians of 3 runs)

Producer cost, ns/event:

| Build | Large ring (256 MiB) | Cache-sized ring (256 KiB) |
|---|---|---|
| Baseline (32 B), no args | 69.9 | 42.1 |
| Args tree (48 B), no args | 90.1 | 43.7 |
| Args tree (48 B), 2 args | 90.5 | 44.5 |

Raw runs:

| Run | Baseline large | Baseline cached | New large (no args) | New cached (no args) | New large (2 args) | New cached (2 args) |
|---|---|---|---|---|---|---|
| 1 | 70.4 | 45.7 | 92.5 | 46.2 | 92.9 | 44.9 |
| 2 | 66.8 | 42.0 | 90.1 | 43.4 | 90.5 | 44.5 |
| 3 | 69.9 | 42.1 | 89.5 | 43.7 | 89.1 | 44.3 |

## Findings

- With no args, the 48-byte record costs about 25–30% more per producer call
  when the ring is large (69.9 → 90.1 ns in run set B; 68.9 → 93.5 ns in run
  set A). The investigation below shows that this gap is first-touch page
  faults, not the slot size. A cache-sized ring has few faults, so its 4% gap
  (42.1 → 43.7 ns) does not isolate the slot size either.
- Once the ring is warm, the 48-byte record costs about 6% more (44.7 → 47.5
  ns). That remaining cost is the bigger slot's writes, plus the two extra
  stores that zero the arg fields. It is not separated further.
- The drain cost is unchanged for events without args, and so is the
  on-disk size.
- Two args add about 2 ns to the producer, since packing is cheap. They add
  about 64 ns per event in the drain, which is the formatting work.
- Args add 12.5 bytes per event on disk, from the rendered name text.

## Investigation: where the 25% comes from (run set C, medians of 3 runs)

Run set B's large-ring numbers were written into a freshly allocated ring, so
every page was faulted in during the timed loop. The benchmark now counts the
minor page faults during the timed loop, and it can write the whole ring once
before timing (`warm`).

| Build | Ring | Minor faults | Producer, ns/event |
|---|---|---|---|
| Baseline (32 B), no args | cold, 256 MiB | 15,625 | 82.8 |
| Baseline (32 B), no args | warm | 0 | 44.7 |
| Args tree (48 B), no args | cold, 256 MiB | 23,437 | 102.2 |
| Args tree (48 B), no args | warm | 0 | 47.5 |
| Args tree (48 B), 2 args | warm | 0 | 44.1–49.1 (3 runs) |

The fault counts match the bytes exactly: 2,000,000 events × 32 B / 4 KiB =
15,625 pages, and × 48 B / 4 KiB = 23,437 pages.

Cost per fault implied by the cold/warm gap: about 4.9 µs for the baseline and
4.7 µs for the args tree. The 25% cold gap is almost all first-touch page
faults, which cost more than the writes themselves on this VM.

Once the ring is warm, the 48-byte record costs about 6% more per producer call
(44.7 → 47.5 ns). That is the remaining effect of the bigger slot: writes, not
faults. The cache-sized ring does not separate the two causes: it has few faults
and also less memory traffic, so its 43.9 vs 44.7 ns gap is not a measure of
either one alone.

Consequence: in a real process the faults happen once, on the first pass over
each page. A long-running trace reaches steady state quickly. A ring that is
touched up front (at open) would move that cost out of the hot path. Not done.

## Warm-up at open (run set D, medians of 3 runs)

The MPSC ring is now written once at open (`warm_ring()`, `src/pf_ring_mpsc.c`),
so the first pass over each page is paid at open, not on the producer. The
baseline tree got the same change, so both builds are compared like for like.

| Build | Open, 256 MiB ring | Minor faults during produce | Producer, cold, ns/event | Producer, cached 256 KiB, ns/event |
|---|---|---|---|---|
| Baseline (32 B), no args | 758 ms | 0 | 23.7 | 40.8 |
| Args tree (48 B), no args | 966 ms | 0 | 26.2 | 43.4 |
| Args tree (48 B), 2 args | 968 ms | 0 | 27.3 | 43.9 |

With the warm-up, the producer gap for the 48-byte record is about 10%, down
from about 25% cold. The remaining gap is the bigger slot's writes and the
extra stores, as before.

Open time rose by about 27%. Part of that is the extra page faults for a larger
ring, which the rounding below makes worse.

Slot count rounding: the ring holds at least `min_ring_bytes`. It is sized in
slots, `min_ring_bytes` divided by the slot size rounded up, and that count is
rounded up to a power of two. For 256 MiB:

- 32-byte slots: 2^23 slots, 256 MiB exactly.
- 48-byte slots: 256 MiB / 48 rounds up to 2^23 slots, 384 MiB.

So the 48-byte build was measured with 1.5× the memory of the baseline for the
same `min_ring_bytes`. This is the rounding, not a bug: the ring never goes
below the minimum. Choosing a size that fits the memory budget is the user's
job; the library does not round down. A 64-byte slot is a power of two and
does not have this problem.

## Not measured

- **Drop rate under load.** A 4-thread burst with a 64 KiB ring dropped only
  0.01–0.02% in both builds, which is noise. The single drain thread keeps up
  with these producers here, so the test can't show a capacity effect. It
  needs a workload that outruns the drain. Removed from the benchmark.
- **Same slot count.** A comparison at equal slot count, rather than equal
  bytes, would separate the cost of the bigger slot from the loss of
  capacity. The library rounds `min_ring_bytes` up to a power of two, and 48-byte
  slots don't divide a power of two evenly, so the slot counts can't match
  exactly. Not attempted.

## Clock read cost on bare metal

The 8.5 µs clock read above is from the VM. Measured on a bare-metal host
(Intel i7-12700, `systemd-detect-virt` = none, clocksource `tsc`):

| Measurement | Cost |
|---|---|
| vDSO `clock_gettime(CLOCK_MONOTONIC)`, the path `now_ns()` uses | 13 ns/call |
| `clock_cost_ns()` in `tests/bench_args.c` (direct syscall) | 105–118 ns/call |

The 13 ns figure is the one that matters for the library. `clock_cost_ns()`
calls `syscall(SYS_clock_gettime)` directly and so skips the vDSO. It measures
the syscall path, so it overstates the library's cost and is an upper bound.

A 13 ns read is about the same as the per-event encode cost, so the library
does not need a direct TSC read. The 8.5 µs figure is a property of that VM's
clock source, not of the library. The producer numbers in this document are
still with the clock stubbed; with a 13 ns read added, they would shift by
about that much.

## Caveats

- The clock is stubbed, so the producer numbers exclude the real clock read.
  On the VM it is about 8.5 µs; on bare metal it is about 13 ns (see above).
- The benchmark runs on a single machine, in a sandbox.
