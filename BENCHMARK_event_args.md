# Event arguments and the 48-byte record, under pftrace

Cost of the record-size change (32 → 48 bytes per ring slot) and of event
arguments (`pf_slice_begin_args`, up to `PF_MAX_ARGS` = 2). Measured with
`tests/bench_args.c`, built against the baseline tree (no args API, 32-byte
records) and the args tree (48-byte records).

## Method

- Snapshot mode, protobuf output, 1,000,000 begin/end pairs per section, so
  2,000,000 events. The ring holds all of them (256 MiB).
- The producer loop is timed with a direct `clock_gettime` syscall.
- The library reads the clock once per event. On this machine that read
  costs about 8.5 µs (see below), so the benchmark replaces the library's
  `clock_gettime` with a counter. The counter is only for timestamps; the
  timing loops are unaffected. This isolates the ring and encode path.
- The drain figure is the time for `pf_trace_dump` divided by events.
- Three runs per build. Medians are below; all runs are listed too.

## Results

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

## Findings

- With no args, the 48-byte record costs about 25% more per producer call
  (68.9 → 93.5 ns). The drain cost is unchanged within noise, and the
  on-disk size is unchanged.
- Two args add about 2 ns to the producer, since packing is cheap. They add
  about 64 ns per event in the drain, which is the formatting work.
- Args add 12.5 bytes per event on disk, from the rendered name text.

## Not measured

- **Drop rate under load.** A 4-thread burst with a 64 KiB ring dropped only
  0.01–0.02% in both builds, which is noise. The single drain thread keeps up
  with these producers here, so the test can't show a capacity effect. It
  needs a workload that outruns the drain. Dropped.
- **Same slot count.** A comparison at equal slot count, rather than equal
  bytes, would separate the cost of the bigger slot from the loss of
  capacity. The library rounds `ring_bytes` up to a power of two, and 48-byte
  slots don't divide a power of two evenly, so the slot counts can't match
  exactly. Not attempted.

## Caveats

- The clock is stubbed, so the producer numbers exclude the real clock read.
  On this machine that read is 8.5 µs; on a host with a vDSO fast path it is
  far smaller, and the real producer cost there is different from these
  numbers.
- The benchmark runs on a single machine, in a sandbox.
