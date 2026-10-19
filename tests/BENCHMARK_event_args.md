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
  set A). With a cache-sized ring, the gap is about 4% (42.1 → 43.7 ns).
  The extra cost is memory traffic from the bigger slot: 48 bytes of writes
  per event instead of 32, which is 1.5× the bytes once the ring no longer
  fits in cache. The small remaining gap matches the two extra stores that
  zero the arg fields.
- The drain cost is unchanged for events without args, and so is the
  on-disk size.
- Two args add about 2 ns to the producer, since packing is cheap. They add
  about 64 ns per event in the drain, which is the formatting work.
- Args add 12.5 bytes per event on disk, from the rendered name text.

## Not measured

- **Drop rate under load.** A 4-thread burst with a 64 KiB ring dropped only
  0.01–0.02% in both builds, which is noise. The single drain thread keeps up
  with these producers here, so the test can't show a capacity effect. It
  needs a workload that outruns the drain. Removed from the benchmark.
- **Same slot count.** A comparison at equal slot count, rather than equal
  bytes, would separate the cost of the bigger slot from the loss of
  capacity. The library rounds `ring_bytes` up to a power of two, and 48-byte
  slots don't divide a power of two evenly, so the slot counts can't match
  exactly. Not attempted.

## Caveats

- The clock is stubbed, so the producer numbers exclude the real clock read.
  On this machine that read is about 8.5 µs. On a host with a fast clock read
  the real producer cost is different from these numbers.
- The benchmark runs on a single machine, in a sandbox.
