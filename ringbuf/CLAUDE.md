# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Toolchain

No compiler, CMake, or clang-format exists on the bare `PATH` here. Prefix
build/test commands with the project's dev shell:

```bash
nix shell ~/project/nixos_config/.#cshell --command <cmd>   # cmake 4.1.6, clang, ninja
nix shell nixpkgs#clang-tools --command clang-format ...     # not in cshell
```

## Build and test

```bash
cmake -B build                                  # add -DRINGBUF_BUILD_TSAN=ON for the tsan target
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Two CTest entries: `ringbuf` and (opt-in) `ringbuf_tsan`. Select one with an
anchored regex — `-R ringbuf` matches both:

```bash
ctest --test-dir build -R '^ringbuf$'
```

With the mutex, MPSC, reserve/submit and variable-length variants there are now
**twelve** entries — `ringbuf`, `ringbuf_mutex`, `ringbuf_mpsc`,
`ringbuf_zcopy`, `ringbuf_zcopy_mpsc`, `ringbuf_vslot`, and the six opt-in
`*_tsan` ones — so `-R ringbuf` matches all twelve and the anchored form matters
more than before.

Note that `ringbuf_zcopy_mpsc` is **not** a sixth implementation: it is a second
test binary over the `ringbuf_zcopy` library, covering the multi-producer
discipline that library permits. There are five `.c` implementations and six
non-tsan CTest entries. See "Several producers under a caller-held mutex".

`build/` already holds a **Unix Makefiles** cache. `-G Ninja` will refuse to
reconfigure over it; `rm -rf build` first if you want to switch generators.

`ringbuf_vslot` and its test targets are **gated on Linux** (`memfd_create`), with
a `message(STATUS)` skip rather than a `FATAL_ERROR` — unlike the benchmarks,
which are opt-in and so can fail loudly. On a non-Linux host the build succeeds
with ten CTest entries instead of twelve. `ringbuf_mpsc`, `ringbuf_zcopy` and
`ringbuf_zcopy` are plain C11 + POSIX and are **not** gated.

### CI

`.github/workflows/tests.yml` runs on every push and PR. Two jobs:

- **`test`** — a four-way matrix, `{x86-64, arm64} x {gcc, clang}`, configuring
  with `-DRINGBUF_BUILD_TSAN=ON` and `-DCMAKE_C_FLAGS=-Werror`, then the full
  `ctest` with `--timeout 300`. arm64 uses the native `ubuntu-24.04-arm`
  runner.
- **`format`** — `clang-format --dry-run --Werror *.c *.h`, plus a `grep` for
  the no-struct-typedef rule that clang-format cannot express.

Four things about it are deliberate:

- **arm64 is the sharpest gate in the matrix, not a portability courtesy.**
  x86-64 is TSO, so the acquire/release pairs this tree depends on compile to
  *plain `mov`s* — a store that should carry `release` but was left `relaxed`
  usually still behaves there. aarch64 compiles them to real instructions and
  genuinely reorders without them. Measured, cross-compiling at `-O2`:
  `ringbuf.o` emits 7 `ldar` / 5 `stlr`, `ringbuf_vslot.o` emits 7 `ldar` /
  9 `stlr`, while the x86-64 object has **zero** fences for the same source
  (only 4 `lock` ops, for the CAS and fetch-add that need them anyway). Since
  CLAUDE.md describes that pair as the only ordering doing real work, arm64 is
  where getting it wrong stops being invisible. Native runners, not qemu —
  emulation does not faithfully model the relaxed memory model, so it would
  not reproduce the reorderings this is here to catch.
- **Both compilers, because they do not warn about the same things.** GCC's
  `-Wmaybe-uninitialized` caught a real bug in `test_ringbuf_vslot.c` that
  clang was silent on (`message_ok` returning early without setting `*seq`,
  which the caller's error path then printed). A single-compiler matrix would
  have shipped it.
- **tsan is always ON in CI**, though it is opt-in locally. It is the gate on
  the lock-free ordering, so CI is where it should not be skippable.
- **`-Werror` in CI only, never in the local build.** Locally it turns an
  unrelated toolchain quirk into a hard stop mid-task; CI wants the opposite
  bias. That is why the flag is passed at configure time rather than added to
  `RINGBUF_WARNINGS` in `CMakeLists.txt`.

Nothing is `apt-get install`ed: both runner images already ship gcc, clang,
clang-format and CMake (checked against `actions/runner-images`). The jobs
print the compiler and `uname -m` instead, so a future image change shows up
as an obvious diff rather than a mystery failure.

The `--timeout 300` is a backstop, not a tuning knob — the suite is ~15 s on
x86-64. The tests' own `MAX_SPIN` bounds a *stalled* push/pop, but a
corrupted-yet-progressing stream resets that counter on every success, so a
regression of that shape would otherwise hang the runner for the full six
hours. A deliberate-break check during development did exactly that.

If the arm64 tsan entries ever abort with `unsupported VMA range`, that is a
known ThreadSanitizer/aarch64 limitation on 39-bit-VA kernels rather than a
bug here; the current runners are 48-bit-VA Azure hosts, so it should not
arise, and the six non-tsan entries still gate the logic regardless.

### Benchmark

Opt-in, and deliberately **not** CTest entries — they are measurements, not
pass/fail checks, and they run for tens of seconds to minutes:

```bash
cmake -B build -DRINGBUF_BUILD_BENCH=ON && cmake --build build -j
./build/bench_ringbuf                        # ringbuf vs an internal toy mutex ring
./build/bench_ringbuf_vs_ringbuf_mutex       # the two real SPSC variants (~4 min)
./build/bench_ringbuf_mpsc_vs_ringbuf_mutex  # multi-producer fan-in (~4.5 min)
```

All three are always compiled `-O2` regardless of `CMAKE_BUILD_TYPE`, so a
stray Debug configure cannot silently produce meaningless numbers. Linux-only:
they pin threads to fixed CPUs with `pthread_setaffinity_np`, without which a
mid-run migration shows up as a throughput cliff unrelated to the code. CMake
fails at configure time on other platforms.

`bench_ringbuf.c` measures `ringbuf` against an internal `struct mq` baseline —
a mutex-protected ring with the *same* non-blocking contract (lock, check, copy,
unlock; `false` when full/empty). That baseline is intentionally not a condvar
blocking queue: the point is to isolate the cost of the lock, not futex wakeup
latency. It is a separate toy struct, unrelated to `ringbuf_mutex.{c,h}`.

`bench_ringbuf_vs_ringbuf_mutex.c` is the one that measures the two **real**
implementations against each other, linking both (which is what the prefixed
symbols are for) and reporting CPU consumption alongside throughput. Results
and analysis are written up in `BENCHMARK_ringbuf_vs_ringbuf_mutex.md`. Two
measurement traps it is built around, both of which flatter `ringbuf` if
ignored:

- **Overwrite-mode throughput is not a win condition.** Evicting is cheaper
  than delivering, so a variant can post a higher *push* rate purely by
  dropping more. Hence the separate `recv-Mm/s` (delivered) column and the
  per-delivered-message CPU normalization. Scenario [5] has `ringbuf` pushing
  2.3× faster while delivering only 1.3× as much.
- **Saturated CPU numbers are not independent of throughput.** When both
  threads spin flat out `cpu/wall` pins at ~1.95 and CPU-per-message is just
  2× the wall cost. Scenario [6] paces the offered rate below saturation,
  which is the only configuration where the CPU columns add information.

Each row reports the single repetition whose throughput was the *median*, not a
per-column median, so every column of a row describes one coherent run.

Headline result: the variants are equivalent in lossless mode (identical code
path, no lock either side), `ringbuf_mutex` is up to 2.9× faster in contended
overwrite mode, and `ringbuf` wins only when saturated and dropping ~94% of
traffic. Notably `ringbuf`'s overwrite-mode cost is on the consumer's hot path
rather than in locking: `ringbuf_pop` does two atomic RMWs (slot-lock CAS plus
`read_idx` CAS) on *every* pop, measured at 2.45× slower uncontended with no
lock involved on either side.

`bench_ringbuf_mpsc_vs_ringbuf_mutex.c` is the multi-producer one, written up
in `BENCHMARK_ringbuf_mpsc_vs_ringbuf_mutex.md`. **It is the one benchmark here
whose two sides do not share a contract**, and that drives its whole shape:
`ringbuf_mpsc_push` takes N concurrent producers, `ringbuf_mutex_push` takes
exactly one, so two threads calling the latter is UB rather than a slow result.
The baseline is therefore what a caller would have to write — `ringbuf_mutex`
plus their own producer mutex (`lock / one push attempt / unlock`), labelled
`ringbuf_mutex+mu`. That wrapper is legal because the SPSC contract forbids
*concurrent* pushers, not a rotating producer identity: `cached_read` and the
release store to `write_idx` pass between producers through the external
mutex's own acquire/release edges. Three things about it are deliberate:

- **The retry loop is outside the push lock.** Spinning on a full buffer while
  holding it would block every other producer behind a condition only the
  consumer can clear — a bug in the wrapper, not a property of the variant.
- **A `finish spread` column** (slowest producer's wall time ÷ fastest's) that
  the SPSC benchmark has no use for. With N producers on one queue, fairness is
  a property of the synchronization, and it is the one axis the mutex wins.
- **Failed pushes/pops yield after 64 consecutive failures** rather than
  spinning forever. Load-bearing for the oversubscribed scenario: `ringbuf_mpsc`
  can have a producer holding an unpublished ticket, which blocks delivery
  head-of-line until it is rescheduled, so a pure spin there is not a
  measurement. Applied identically to both sides.

Its `cpu/wall` saturates near `producers+1`, not 2.0, so **its numbers are not
comparable with the SPSC benchmark's** even where the scenarios look alike.

Headline result: `ringbuf_mpsc` wins 1.45–3.0× on delivered throughput at every
producer count from 2 to 16, with the margin *widening* to 3.0× at 1024B slots
(the mutex wrapper holds its lock across the memcpy; the ticket design copies
concurrently). Oversubscription at 16 producers on 7 CPUs does not flip it.

Two reversals are the interesting part, and both are worth not rediscovering:

- **At exactly one producer in overwrite mode `ringbuf_mutex+mu` wins 1.40×**,
  because there is no fan-in to parallelize and what remains is
  `pop_overwrite`'s three atomic RMWs per pop plus the ticket CAS — isolated at
  ~14.5 ns/op uncontended. Same shape as `ringbuf`'s finding above.
- **With a slow consumer and ~70% drops, `ringbuf_mpsc` delivers 16–42% less**
  while posting a higher *push* rate, burning ~2200 failed pushes per 1000
  messages. This **inverts** the SPSC benchmark's equivalent scenario, where
  lock-free fail-fast won. The generalizable reason: with several producers,
  retries contend with each other, so "fail fast and spin" stops being cheap.

### Running a single test case

Not supported. `main(void)` in `test_ringbuf.c` and `test_ringbuf_mutex.c` takes
no arguments and runs all 7 cases unconditionally (`test_ringbuf_vslot.c` and
`test_ringbuf_mpsc.c` each run all 10, `test_ringbuf_zcopy.c` all 14, and
`test_ringbuf_zcopy_mpsc.c` all 5); there is no filter flag. To isolate one,
comment out the others in `main` or add arg parsing.

The two stress tests push 2,000,000 messages each and dominate the ~0.2 s
runtime; the tsan build takes ~2 s. `ringbuf_mpsc` is the slowest entry —
~0.8 s, and ~8 s under tsan — since its two multi-producer stress cases run four
pushing threads on a capacity-4 ring. `ringbuf_zcopy_mpsc` is close behind at
~2 s and ~10 s under tsan: all five of its cases are concurrent, four of them
running four producers against a capacity-4 ring. `ringbuf_zcopy` follows at
~0.4 s and ~4 s under tsan, for the same reason: its three `parallel_fill*`
cases each run a reserver, four filler threads and a consumer.

## Two implementations

`ringbuf.{c,h}` is the lock-free original. `ringbuf_mutex.{c,h}` is a sibling
with the **same data structure and the same external SPSC contract**, differing
only in how overwrite mode reconciles its two writers to `read_idx`. Symbols are
prefixed (`ringbuf_mutex_push`, `struct ringbuf_mutex`, …) so both can link into
one binary.

(There are also three further variants, each changing a different axis:
`ringbuf_vslot.{c,h}` changes the *storage model* — see "The variable-length
variant" — `ringbuf_mpsc.{c,h}` changes the *contract*, allowing many
producers (see "The multi-producer variant"), and `ringbuf_zcopy.{c,h}` changes
the *operation*, splitting `push` into reserve and submit so the payload is
written in place and several writes can overlap. See "The reserve/submit
variant".)

| | `ringbuf` | `ringbuf_mutex` |
| --- | --- | --- |
| Overwrite-mode sync | per-slot spinlocks + `read_idx` CAS | one `pthread_mutex_t` |
| `push` on full | may return `false` transiently | **always** `true`; may block |
| `pop` returning `false` | empty **or** transient contention | empty, and nothing else |
| Caller retry loop | required on both sides | consumer only |
| Lock-free / signal-safe | yes | **no** — `push` can block |
| Lossless mode | lock-free | identical, lock-free, no mutex initialized |

Pick `ringbuf` when push must never block (signal handlers, hard real-time);
pick `ringbuf_mutex` when it may and you want readable code. The sections below
describe `ringbuf.c`; everything about counters, wrapping, capacity rounding, and
the single allocation applies to both. **`ringbuf_vslot` is the exception on every
one of those** — byte counters, a mirrored mapping, byte capacity, and two
allocations — so read its own section rather than assuming these carry over.
`ringbuf_mpsc` keeps all of them, and breaks the single-producer premise instead.
`ringbuf_zcopy` keeps all of them too, and breaks the premise that a message is
*stored* by the same call that claims its slot — and, given a caller-held mutex,
the single-producer premise as well.

## Architecture

Lock-free ring buffer, SPSC in three of its five variants. `ringbuf.{c,h}` and
`ringbuf_mutex.{c,h}` are interchangeable fixed-size-slot implementations (see
"Two implementations" above); `ringbuf_vslot.{c,h}` does variable-length
messages (see "The variable-length variant"); `ringbuf_mpsc.{c,h}` takes many
producers (see "The multi-producer variant"); `ringbuf_zcopy.{c,h}` splits
`push` into reserve and submit, and takes many producers when the caller
serializes them (see "The reserve/submit variant" and "Several producers under
a caller-held mutex"). `test_ringbuf.c` and `test_ringbuf_mutex.c` mirror each
other case for case, `test_ringbuf_vslot.c` adds three cases of its own,
`test_ringbuf_mpsc.c` adds three, and `test_ringbuf_zcopy.c` adds seven;
`test_ringbuf_zcopy_mpsc.c` is a second binary over the same `ringbuf_zcopy`
library with five multi-producer cases. Three benchmarks sit alongside
(`bench_ringbuf.c`, `bench_ringbuf_vs_ringbuf_mutex.c`,
`bench_ringbuf_mpsc_vs_ringbuf_mutex.c`). CMake builds a static library per
variant plus the test binaries.

Exactly one thread may call `ringbuf_push`, exactly one may call `ringbuf_pop`.
More than one on either side is UB — this is the premise the whole design rests
on, not a caveat. **`ringbuf_mpsc` and `ringbuf_zcopy` are the two variants that
lift this**, and only on the push side; both consumers are still single-caller.
They lift it differently, which is the whole distinction between them:
`ringbuf_mpsc` synchronizes the producers itself, `ringbuf_zcopy` requires the
caller to do it with a mutex of its own.

**Two modes, set at create time.** With `overwrite_on_full = false`, a push onto
a full buffer fails. With `true`, it evicts the oldest unread slot instead and
counts it in `ringbuf_dropped_count`.

**Overwrite mode breaks the clean SPSC split, and that drives most of the
complexity.** In lossless mode `read_idx` has exactly one writer (the consumer)
and a plain release store suffices. In overwrite mode the *producer* also
advances `read_idx` when it evicts, so there are two writers. Two mechanisms
handle that, and both must hold together:

- **Per-slot locks** (`slot_lock`, allocated only in overwrite mode) guard the
  one physical slot where an in-flight pop can overlap with an evicting push.
  Either side failing to take the lock returns `false` — a transient contention
  signal, not an error. Both `push` and `pop` can therefore spuriously fail in
  this mode, and **every caller must retry**; the tests use bounded spin loops.
- **`read_idx` advances only via `CAS(x -> x+1)`** on both sides. This is what
  makes a message either received or dropped but never both, and keeps
  `read_idx` from moving backwards over a concurrent eviction (which would hand
  out the same message twice).

### SPSC at the API, multi-writer `read_idx` inside

These are two separate claims and it is easy to conflate them. The internal
contention on `read_idx` does **not** make the buffer SPMC, and a reader who
infers otherwise from the eviction CAS will ship a race.

- **Internally**, `read_idx` is a multi-writer retirement counter. Two threads
  race to retire message *n* via `CAS(n -> n+1)`: the producer evicting (in
  `ringbuf_push`) and the consumer receiving (in `ringbuf_pop`). Exactly one
  wins per *n*, which is where `received + dropped == pushed` comes from.
  "MPSC on `read_idx`" describes this more tightly than "SPMC" — the extra
  writer is the producer, not an extra consumer.
- **Externally**, `pop` is still single-caller. `read_idx` being CAS-safe is
  not sufficient, because `pop` also reads and writes `cached_write`, a plain
  non-atomic `size_t`. One consumer owns it exclusively; two consumers race on
  it immediately (tsan flags it on the first contended pop). The producer never
  touches it, so the multi-writer `read_idx` costs nothing there.

The asymmetry is deliberate: the producer writes consumer-owned state, but never
the reverse. `write_idx` stays single-writer in both modes, which is why it keeps
a cheap relaxed-load / release-store pair while `read_idx` needs the CAS.

Real SPMC would mean dropping or atomicizing `cached_write` and auditing the
slot locks for consumer-vs-consumer overlap, which they were never designed for.
The eviction machinery is not the obstacle.

All of the above is about **`ringbuf.c`**, where `write_idx` is single-writer.
It is not the general shape of this tree: `ringbuf_mpsc.c` makes `write_idx`
multi-writer on purpose and pays the CAS for it. Do not carry the
"`write_idx` stays single-writer" claim across to that file.

Nor should you infer the converse — that taking several producers *requires*
making it multi-writer. `ringbuf_zcopy.c` takes N producers — given a
caller-held mutex — and keeps `write_idx` single-writer (relaxed both ways, no
CAS), because that mutex means only one store is ever in flight.
"Multi-producer" and "multi-writer `write_idx`" are independent properties
here.

The *reason* `pop` is single-caller does not carry across either.
`ringbuf_zcopy.c` keeps `write_idx` single-writer but has **no `cached_write`**
at all — the stamp subsumes the empty check — so the argument above, that two
consumers would race on that plain `size_t`, does not apply there. What makes
its `pop` single-caller is `read_idx` itself: stored plainly by one writer, so
two poppers would both read the same index and deliver the same message twice.

### The mutex variant's two invariants

`ringbuf_mutex.c` replaces both mechanisms above with one lock held by every
`pop` and by `push`'s eviction branch only. The hot push path stays unlocked,
because when the buffer is not full `write_idx & mask != read_idx & mask` — the
indices alias only when full, which is exactly the branch that locks. Two
details are load-bearing and both are verified by deliberately breaking them:

- **`pop` re-reads `read_idx` *after* acquiring the lock**, never reusing a
  value sampled before it. While it waited, the producer may have evicted the
  message it was about to take, counted it in `dropped_count`, and refilled the
  slot; acting on the stale index hands out that new message as the evicted one.
  Breaking this fails `overwrite_stress` with duplicate/out-of-order seqs — but
  **only** in the concurrent cases, so the single-threaded eviction tests pass
  and cannot be relied on to catch it.
- **`pop`'s `read_idx` store stays `memory_order_release` and `push`'s
  fullness load stays `acquire`.** A mutex only orders you against threads
  taking the same mutex, and `push`'s fast path never takes it — so the
  consumer's unlock gives the producer no edge. That acquire/release pair,
  *not* the lock, is what makes the consumer's finished read visible before
  the producer overwrites the slot. `ringbuf_mutex_tsan` is the gate on this:
  relaxing either end reports a race on the slot data (`__tsan_memcpy`), not
  on `read_idx` itself.

  The corollary, and the easy thing to get backwards: the two stores inside
  `push`'s eviction branch are deliberately **relaxed**, because that branch
  is overwrite-mode-only and there `pop` touches both indices under the same
  lock, so the unlock/lock edge already covers them. Every ordering in the
  file that is *not* relaxed has an unlocked access on at least one end; if
  both ends lock, relaxed is correct. Promoting those two back to `release`
  is harmless at runtime but muddies that rule.

Because `pop` can no longer fail transiently, the overwrite-mode producers in
`test_ringbuf_mutex.c` have **no retry loop**: a `false` from push is a hard
test failure, which is the variant's main observable guarantee.

## The multi-producer variant

`ringbuf_mpsc.{c,h}` is the only variant that changes the **contract** rather
than the implementation behind it: any number of threads may call
`ringbuf_mpsc_push` concurrently. The consumer is still single-caller. Plain
C11 + POSIX, so unlike `ringbuf_vslot` it is not Linux-gated, and everything
in "Architecture" about counters, wrapping, capacity rounding and the single
allocation carries over unchanged.

| | `ringbuf` | `ringbuf_mpsc` |
| --- | --- | --- |
| Producers | exactly one | **any number** |
| `write_idx` | single-writer, relaxed load + release store | **multi-writer**, ticket CAS |
| Publication | `write_idx` itself marks a slot ready | **per-slot stamp** |
| Fullness check | compare `write_idx` against `cached_read` | the stamp subsumes it; producers never read `read_idx` |
| `pop` returning `false` | empty, or transient contention (overwrite only) | also **head-of-line**: oldest ticket claimed but not yet published |
| Lock-free / signal-safe | yes | yes, both modes |

### Tickets and stamps

A producer claims slot `w` by winning `CAS(write_idx: w -> w+1)` — the ticket.
It then copies its message and release-stores `stamp[w & mask] = w + 1` to
publish. One `atomic_size_t` stamp per slot, initialized `stamp[i] = i`:

```
stamp == w      free, reserved for the producer holding ticket w
stamp == w + 1  published, holds ticket w's message
```

so a slot's life cycle is `i` → `i+1` → `i+capacity` → `i+capacity+1` …,
gaining `capacity` each lap. **Compared as a signed difference against a
ticket, never absolutely**, so it wraps past `SIZE_MAX` like every other
counter here.

**The stamp subsumes the fullness check**, which is why there is no
`cached_read` in this struct: a producer compares `stamp[w & mask]` with its
own `w`, and a negative difference means a previous lap's message is still
unread. Producers never read `read_idx` at all.

### What is load-bearing, and how it was established

Each of these was confirmed by **deliberately breaking it** and checking that
the predicted failure appears — the same method the other variants' notes rest
on.

- **The slot lock is taken BEFORE the ticket CAS** in the eviction path, and
  that order is the subtle point in the file. A won ticket is irrevocable, so
  claiming it first and then failing to take the lock would leave a permanent
  hole in the sequence at `w` that the consumer waits on forever. Locking on a
  merely tentative `w` is safe because the ticket CAS still validates it and
  retries on a stale value. Breaking this wedges `overwrite_stress` and
  `mpsc_overwrite_stress` — both sides stall at `MAX_SPIN`.
- **An evicting producer must advance `read_idx`, not just the stamp.** The
  stamp CAS decides *who* retires a message; `read_idx` is what tells the
  consumer where the live window starts. Bumping only the stamp leaves the
  consumer waiting on a ticket that no longer exists in the ring: the failure
  is `received=0` with a drop count climbing to the full push total. So
  `read_idx` has N+1 writers in overwrite mode and both sides go through
  `advance_read_idx()`, a store-max CAS loop rather than a fetch-add — the two
  sides can both try to retire the same message and the loser must not
  double-count.
- **The stamp's release store / acquire load pair is the only ordering doing
  real work**, as in every other variant. Relaxing the publish store makes
  `ringbuf_mpsc_tsan` report a data race on the slot bytes. Trust this one
  least on x86-64, where TSO compiles both to plain `mov`s — the arm64 CI legs
  are the real gate.
- **`ringbuf_mpsc_test_seed_counters` must re-stamp every slot**, to the
  ticket each slot awaits (`stamp[t & mask] == t`, derived from the masked
  offset since `start` is not a multiple of the capacity). The stamps hold
  absolute ticket numbers, so leaving them at create-time values desynchronizes
  them from the seeded counters, every slot reads as another lap's, and `push`
  spins forever — even single-threaded `wrap_boundary` **hangs** rather than
  failing a check.

By contrast, the **receive-commit CAS in `pop_overwrite` is not load-bearing**:
replacing it with a plain store leaves the whole suite passing. Both the
evicting producer and the consumer take the same `slot_lock[idx]` before
touching the stamp, so the lock — not the CAS — is what makes a message
counted as dropped or received and never both. It is kept for symmetry with
the eviction path and as a cheap assertion. This is the opposite of
`ringbuf.c`, where the `read_idx` CAS genuinely is the mechanism, so do not
reason by analogy between the two files.

### Delivery order

Messages are ordered by **ticket**, not by call time: two producers racing in
`push` may be delivered in either order. Each producer's own messages do stay
in the order it pushed them. `test_ringbuf_mpsc.c` therefore cannot check a
single global sequence — `struct message` carries a `producer_id`, and the
consumer checks **per-producer** strict monotonicity, which is what a broken
ticket CAS destroys. In lossless mode it additionally requires each stream to
arrive complete.

`test_ringbuf_mpsc.c` runs **ten** cases: the seven mirroring `test_ringbuf.c`
(single-producer, so they stay directly comparable) plus `mpsc_lossless`,
`mpsc_overwrite_stress`, and `mpsc_wrap_overwrite_stress`, all with
`NUM_PRODUCERS = 4`.

There **is** a benchmark target, unlike `ringbuf_vslot`:
`bench_ringbuf_mpsc_vs_ringbuf_mutex`, measuring this variant against
`ringbuf_mutex` under a caller-held producer mutex, with a producer-count
scaling axis. See "Benchmark" above for what is deliberate about it and
`BENCHMARK_ringbuf_mpsc_vs_ringbuf_mutex.md` for the results.

## The reserve/submit variant

`ringbuf_zcopy.{c,h}` is the only variant that changes the **operation** rather
than the contract, the storage model, or the synchronization. `push` is split
in two:

```c
struct ringbuf_rsv a, b, c;
ringbuf_zcopy_reserve(rb, &a);   /* claim a slot, get a pointer into it */
ringbuf_zcopy_reserve(rb, &b);
ringbuf_zcopy_reserve(rb, &c);

spawn(fill, a.data);             /* three fills run CONCURRENTLY, off-thread */
spawn(fill, b.data);
spawn(fill, c.data);

ringbuf_zcopy_submit(rb, &c);    /* ANY order */
ringbuf_zcopy_submit(rb, &a);
ringbuf_zcopy_submit(rb, &b);
```

Two things that buys: the payload is written **straight into the slot** with no
staging buffer, and several fills **overlap each other** instead of being
serialized by one thread's `memcpy`. Delivery is by **reservation order**
regardless of submit order. Plain C11 + POSIX, so not Linux-gated, and
everything in "Architecture" about counters, wrapping, capacity rounding and the
single allocation carries over unchanged.

Still **SPSC**: one thread reserves, one pops. `submit` is the exception and may
be called from any thread, which is the whole point.

| | `ringbuf_mutex` | `ringbuf_zcopy` |
| --- | --- | --- |
| Storing a message | one `push` call | `reserve` → caller fills → `submit` |
| Outstanding writes | none | up to `capacity`, submitted in **any** order |
| Publication | the release store on `write_idx` | **per-slot stamp** |
| `write_idx` | single-writer, relaxed load + release store | single-writer, **relaxed both ways** |
| Fullness check | compare `write_idx` against `cached_read` | the stamp subsumes it |
| `pop` returning `false` | empty, and nothing else | also **head-of-line**: oldest slot reserved but unsubmitted |
| Caller retry loop | consumer only | **both sides** |

It is `ringbuf_mutex` with one structural change, and the split push forces it.

In `ringbuf_mutex.c`, `write_idx` **is** the readiness marker: the release store
that advances it past a slot is the same event that publishes the message, so
"stored" is exactly the prefix of the ring below `write_idx`. That cannot
survive out-of-order submit — reservations are handed out in ticket order but
completed in any order, so stored slots are no longer a prefix, and a single
monotonic counter cannot describe a ring with holes in it. Readiness therefore
moves to **one stamp per slot**, set by `submit` and tested by `pop`:

```
stamp == w      reserved for ticket w: free, or claimed and not yet submitted
stamp == w + 1  submitted, holds ticket w's message
```

`stamp[i] = i` at create, so a slot's life cycle is `i` → `i+1` →
`i+capacity` → `i+capacity+1` …, gaining `capacity` each lap. **Compared as an
exact equality or a signed difference against a ticket, never as an absolute
magnitude**, so it wraps past `SIZE_MAX` like every other counter here. The
encoding deliberately does not distinguish "free" from "reserved but
unsubmitted" — both are `w` — because the question is always asked about a
slot's *previous* lap, where the two readings differ.

Everything else is `ringbuf_mutex.c` unchanged. One `pthread_mutex_t`, held by
every pop and by `reserve`'s eviction branch only; `write_idx` single-writer;
`read_idx` needing no CAS, because a plain store under that lock is already
single-valued. The stamp replaces the counter comparison for fullness and
emptiness, which is why **neither `cached_read` nor `cached_write` exists** in
this struct.

### The three reserve states

The single producer is what keeps this exhaustive, and it is worth reading as a
consequence of SPSC rather than a convenience. The producer at ticket `w` finds
slot `w & mask` last held `p = w - capacity`:

| stamp | meaning | `reserve` does |
| --- | --- | --- |
| `w` | consumer freed it (or first lap) | take it, unlocked fast path |
| `w - capacity + 1` | `p` submitted, still unread | fail (lossless) / **evict** (overwrite) |
| `w - capacity` | `p` reserved, **not yet submitted** | fail in **both** modes |

Two states when not free, no others — and `stamp > w` in particular is
impossible, since only the holder of reservation `w` can publish `w`, `w` is
handed out by `reserve` alone, and `reserve` is single-caller. So there is no
staleness case and `reserve` needs **no retry loop**: it reads `write_idx` once
and acts on it. A second producer would invalidate that one line of reasoning
and most of what follows from it.

That third row is the variant's one genuinely new failure mode: **an unsubmitted
slot is never evicted**, since a filler may be mid-write. It is about the
reservation's *age*, not how many are outstanding, and only the caller's own
submit can clear it — the consumer cannot. So in overwrite mode `reserve` can
fail, which `ringbuf_mutex_push` never does. **Used only through
`ringbuf_zcopy_push()` the situation cannot arise** (no reservation is ever
outstanding), and the variant is then contract-identical to `ringbuf_mutex`,
including the never-fails promise. That equivalence is what the first seven test
cases assert, mirroring `test_ringbuf_mutex.c` retry-loop-free.

### The ordering spans threads — three edges, not one

Every other variant's acquire/release pair brackets a window **inside a single
`push`**. Here the window spans threads, and correctness rests on a chain:

1. `reserve`'s **acquire** on the stamp pairs with `pop`'s **release** store
   freeing the slot → the consumer's copy-out is complete before the bytes go to
   a filler.
2. The caller's own handoff of the handle to whatever fills it (`pthread_create`,
   a work queue, …) — **supplied by the caller, not by this file.**
3. `submit`'s **release** on the stamp pairs with `pop`'s **acquire** → the fill
   is visible before the consumer copies it out.

Transitivity across all three is what makes it safe, and it holds whether
`submit` runs on the filler thread or back on the reserving thread after a join.
Edge 2 being the caller's responsibility is worth stating to callers: a job
queue handing out `rsv.data` needs its own release/acquire pair in **both**
directions, including when recycling a job slot. The first version of
`test_ringbuf_zcopy.c`'s filler pool got the recycle direction wrong and
`ringbuf_zcopy_tsan` caught it — in the harness, not the library.

`submit` takes no lock in either mode, and that is provable rather than hopeful:
an evicting `reserve` only ever touches a slot whose stamp it has observed as
*submitted*, and a submitted ticket is by definition not the one being
submitted now.

Otherwise the tree's usual rule holds — *every non-relaxed ordering has an
unlocked access on at least one end; if both ends lock, relaxed is correct.*
What is left carrying a non-relaxed ordering is therefore just the stamp, at
both ends, plus the `*_approx` helpers. **Both counters are relaxed in both
modes**, which is weaker than `ringbuf_mutex.c` on each:

- `write_idx`, because nothing reads it for readiness any more — only the
  producer itself, the locked eviction path, and the advisory helpers.
- `read_idx`, because the producer never reads it in lossless mode at all
  (the stamp subsumes the fullness check), and in overwrite mode both ends
  hold the lock. Note the asymmetry with `ringbuf_mutex.c`, where the
  lossless store **must** be release: there the producer does read `read_idx`
  unlocked to test fullness, so that store is what frees the slot. Here the
  release on `stamp[idx]` does that job instead.

Resisting a defensive release on those stores is deliberate. Either would
satisfy the letter of the rule above — the `*_approx` helpers read both
counters without locking — while draining it of meaning, since any store can
find some diagnostic reader. The rule is useful only if "unlocked access"
means one that something load-bearing depends on.

### What is load-bearing, and how it was established

Confirmed by **deliberately breaking each one** and checking the predicted
failure appears, as everywhere else in this tree.

- **`reserve` must re-stamp the slot to `w` when it evicts.** Looks redundant —
  it is about to reserve that slot itself — but the stamp has to name the ticket
  the slot now awaits. Left at `w - capacity + 1`, next lap reads it as
  published-and-evictable while a filler is mid-write. Removing it corrupts
  `parallel_fill_overwrite` and `wrap_parallel_fill` with duplicate seqs; the
  push-only cases **all still pass**, so they cannot be relied on to catch it.
- **A stamp of `w - capacity` must not be treated as evictable.** Caught
  immediately and single-threaded by `unsubmitted_blocks_evict`.
- **`submit`'s release store.** Relaxing it makes `ringbuf_zcopy_tsan` report a
  `__tsan_memcpy` race on the slot bytes between `take_submitted` and the fill
  side — while every logic case still passes. Trust this one least on x86-64
  (TSO compiles it to a plain `mov`); the arm64 CI legs are the real gate.
- **The seed hook must re-stamp every slot.** The stamps hold absolute ticket
  numbers, so leaving them at create-time values desynchronizes them from the
  seeded counters. Disabling the loop fails **all five** `wrap_*` cases,
  including single-threaded `wrap_boundary`.

By contrast, **`pop`'s re-read of `read_idx` under the lock is *not*
load-bearing here**, unlike in `ringbuf_mutex.c` and `ringbuf_vslot.c` where the
identical-looking line is. Sampling it before the lock instead leaves the whole
suite passing, repeatedly. The reason is `take_submitted`'s **exact-equality**
test `st != r + 1`: had the producer evicted message `r` while `pop` waited, the
slot would read `r + capacity` or `r + capacity + 1`, never `r + 1` (capacity is
≥ 2), so a stale index yields at worst a spurious `false` — which this variant's
contract already allows — never a wrong message. `ringbuf_mutex.c` has no such
guard; its `pop` validates only the index against `cached_write`. The re-read is
kept anyway (one relaxed load on a path that already holds the lock, and it
keeps correctness from resting entirely on the stamp encoding), but **do not
carry the "it is load-bearing" claim across from the other two files.** Whether
a given line matters is a property of the file it sits in, not of its shape.

### Tests

`test_ringbuf_zcopy.c` runs **fourteen** cases: the seven mirroring
`test_ringbuf_mutex.c` through `push()` (which is what pins the equivalence
claim), plus `reserve_basic`, `ooo_submit`, `reserve_exhaust`,
`unsubmitted_blocks_evict`, `parallel_fill`, `parallel_fill_overwrite` and
`wrap_parallel_fill`. The three `parallel_fill*` cases run a reserver, four
filler threads that submit from their own threads, and a consumer — they are the
real gate on the three-edge ordering.

Two harness rules differ from every other test file here, both consequences of
head-of-line blocking:

- **A false `pop` never means "drained"** until the producer side is finished
  *and* every filler has joined. The `parallel_fill*` cases set the consumer's
  `done` flag only after `pthread_join` on all fillers.
- `reserve_exhaust` checks that submitting is **not** enough to free a slot —
  only a pop is. A regression conflating "unsubmitted" with "free" would let the
  reserve after a bare submit succeed.

There is deliberately **no benchmark target**, following `ringbuf_vslot`'s
precedent: a fair comparison needs a fill-cost axis and a parallel-fill
workload, which is its own piece of work rather than an extra row in the
existing table.
### Several producers under a caller-held mutex

`ringbuf_zcopy` lifts its own single-producer restriction, and it is the only
variant that lifts one by moving the synchronization **out** of the library:
any number of threads may reserve, provided every reserve is serialized by a
mutex the **caller** supplies. Nothing in `ringbuf_zcopy.c` synchronizes
producers against each other.

```c
pthread_mutex_lock(&my_push_mu);           /* serialize the CLAIM */
bool got = ringbuf_zcopy_reserve(rb, &rsv);
pthread_mutex_unlock(&my_push_mu);

if (got) {
	fill(rsv.data);                    /* OUTSIDE the lock -- every */
	ringbuf_zcopy_submit(rb, &rsv);    /* producer's fill overlaps */
}
```

That is only worth doing because the lock covers so little. Claiming a slot is a
few atomics on fixed state; filling it is a `memcpy`, a `read(2)`, a
serialization pass — unbounded, and the part that costs. A mutex around an
ordinary `push` necessarily covers the payload copy too, serializing exactly the
work you wanted to overlap. That is the whole argument for this arrangement over
`ringbuf_mutex+mu`.

| | single producer | N producers, caller-serialized |
| --- | --- | --- |
| `reserve` | single-caller | single-caller **at a time** |
| `submit` | any thread, no lock | identical — and must **not** need the caller's mutex |
| Unsubmitted slot blocking `reserve` | only your own; retrying is futile | usually a **peer's**; retrying is productive |
| `write_idx` | single-writer by threading | single-writer by **the caller's lock** |

### Why the same code supports both

**There is deliberately no `ringbuf_zcopy_mpsc.c`.** One was written first, as a
separate variant, and came out **token-for-token identical** to
`ringbuf_zcopy.c` once comments and the symbol renaming were normalized away —
same 4716 bytes of code, same header surface. It was deleted rather than
maintained in parallel, since the duplication was total and nothing would have
enforced that a future fix got applied twice.

The reason it came out identical is the point worth keeping. `ringbuf_zcopy.c`
leans on its single producer in exactly three places, and **all three need the
reserves serialized, not same-threaded**:

- `reserve` reads `write_idx` once and acts on it with no retry loop, sound only
  because nobody else can hand out the ticket it just read;
- the eviction path knows its victim is `w - capacity` without searching,
  because tickets are issued *in order* and every older one is already retired —
  an argument about issue order, not about thread identity;
- `write_idx` has one store in flight, so it needs no CAS.

A caller-held mutex supplies all three. Compare `ringbuf_mpsc.c`, which takes
the other branch of the same fork: it makes `write_idx` genuinely multi-writer
with a ticket CAS and pays a store-max CAS on `read_idx`, buying lock-freedom
and signal safety at the cost of copying the payload inside `push`. **Neither
file's reasoning transfers to the other.**

A consequence worth not rediscovering: with producers serialized, `read_idx`
still has only **two** kinds of writer, not N+1 as in `ringbuf_mpsc.c`, because
only one producer can be inside the eviction branch at a time. That is why the
single internal mutex remains sufficient and no store-max loop is needed.

### The fourth edge, and the rules it imposes on the caller

The three-edge chain above is unchanged. What several producers add is a
**fourth edge, also the caller's**: `write_idx` is relaxed at both ends, so
producer B's load observes producer A's store *only* through the caller's
unlock/lock. Relaxed atomics alone promise nothing here.

Three rules follow, and they are the contract:

1. **Hold the mutex across `reserve` only.** Holding it across the fill is
   correct but pointless — that is a serialized push with extra steps.
2. **Never require it on the submit path.** A producer spinning on `reserve` is
   usually blocked by a *peer's* unsubmitted slot, so if that peer needs the
   same mutex to submit, the two deadlock. `peer_unblocks_reserve` demonstrates
   the working arrangement deterministically.
3. **It must be a real lock.** Any `pthread_mutex_t` or correctly built spinlock
   works; a "lock" assembled from relaxed atomics does not, and would compile
   and mostly appear to work.

The counters are deliberately **not** promoted to acquire/release for this.
Doing so would duplicate a mandatory mutex on every reserve, buying nothing,
while implying the counter could carry concurrent producers on its own — which
it cannot.

### What is load-bearing here, and how it was established

The library's own load-bearing lines are listed under "The reserve/submit
variant" and were re-verified against the multi-producer cases; the eviction
re-stamp, the unsubmitted-slot refusal and the seed hook's re-stamp all fail
there too. What is specific to this arrangement:

- **The caller's mutex around `reserve`.** Removing it from
  `gated_reserve` makes `ringbuf_zcopy_mpsc_tsan` report a data race in
  `fill_in_place` — two producers handed the same slot, both writing it — and
  the plain build fails with duplicate seqs within a stream and stalled
  reservers. This is the one claim the binary exists to gate, which is why the
  tsan target's comment calls it out specifically.

### Tests

`test_ringbuf_zcopy_mpsc.c` is a **second test binary over the same library**,
not a second implementation, and runs **five** cases: `mpsc_push`,
`mpsc_parallel_fill`, `mpsc_parallel_fill_overwrite`, `peer_unblocks_reserve`,
and `wrap_mpsc_parallel_fill`. It compiles `ringbuf_zcopy.c` directly, like
`test_ringbuf_zcopy`, because its wrap case needs `RINGBUF_TEST_HOOKS`.

It is a separate file rather than more cases in `test_ringbuf_zcopy.c` because
every reserve and push in it must go through `gated_reserve`/`gated_push`, which
take the mutex. Mixing gated and ungated callers in one file would obscure which
discipline a given case was demonstrating — and the discipline is the thing
under test. So `test_ringbuf_zcopy.c` stays the single-producer contract and
this file is the multi-producer one.

Four things about the harness are deliberate:

- **`payload_key` mixes `seq` *and* `producer_id`.** The single-producer files
  derive payloads from `seq` alone; here the same `seq` occurs once per
  producer, so a seq-only pattern would validate cleanly even if two messages'
  bytes were crossed.
- **`peer_unblocks_reserve` is the case with no single-producer counterpart.**
  Under SPSC a reserve refused for an unsubmitted slot can only be blocked by
  the caller's own reservation, so retrying is futile; with several producers
  the blocker is usually a peer, so retrying is correct. Identical code,
  opposite advice — which is why it is worth a dedicated case.
- **It is a handshake, not a stress test.** A three-phase `atomic_int` pins the
  exact moment of each check, so a failure names the broken transition instead
  of surfacing as corruption under load.
- **`mpsc_push` tests the degenerate configuration on purpose.** It is the one
  multi-producer shape where no reservation is ever outstanding, so overwrite
  mode's never-fails promise must hold for *several* producers — it has no
  retry loop, and a `false` return is a hard failure.

There is deliberately **no benchmark target**, following `ringbuf_zcopy`'s and
`ringbuf_vslot`'s precedent. This is where one would be most interesting — the
natural baseline is `ringbuf_mutex+mu` from
`bench_ringbuf_mpsc_vs_ringbuf_mutex.c`, and the hypothesis is that holding the
lock across the claim rather than the copy is what wins — but it needs a
fill-cost axis to mean anything, which is its own piece of work.

## The variable-length variant

`ringbuf_vslot.{c,h}` is the remaining implementation, and the only one that changes
the **storage model** instead of the synchronization. The other two hand out
fixed-size slots; this one packs **length-prefixed records densely into a byte
ring**, so memory tracks the sizes actually pushed rather than the worst case,
and `pop` reports each message's length.

| | `ringbuf` / `ringbuf_mutex` | `ringbuf_vslot` |
| --- | --- | --- |
| Storage | `capacity` slots of `slot_size` | dense `u32`-prefixed records, 4-byte aligned |
| Sizing | `create(slot_size, min_capacity, …)` | `create(max_msg_size, min_ring_bytes, …)` — **bytes** |
| Counters count | messages, `+1` per op | **bytes**, `+reclen` per op |
| Oversize message | n/a | **truncated** to `max_msg_size`, counted, push still succeeds |
| Physical wrap | cannot straddle a slot | mirrored mapping makes it contiguous |
| Allocation | one `posix_memalign`, one `free` | **two**: control block + `mmap`; `munmap` + `free` |
| Portability | C11 + POSIX | **Linux only** (`memfd_create`) |
| Overwrite-mode sync | per-slot locks + `read_idx` CAS / one mutex | one spinlock, **no CAS** |
| `is_full_approx` | yes | **absent** — replaced by `free_bytes_approx` |

`create`'s two sizes are **independent axes**, and the names are chosen to say
so: `max_msg_size` bounds **one message** (the truncation threshold, and the
receive buffer a consumer needs to never truncate), while `min_ring_bytes`
bounds **all queued messages together** (how far the consumer may fall behind).
Their ratio is only roughly a message count, because each record costs
`4 + len` rounded up to 4 — a 64 KiB ring with `max_msg_size` 1 KiB holds 64
worst-case messages or several thousand 16-byte ones. `min_ring_bytes` is
rounded up to a power of two, a page, *and* one `max_msg_size` record, so a
small request can yield a much larger ring; `capacity_bytes()` reports the
actual figure.

### The mirrored mapping is the core trick

The ring is mapped **twice, back to back**: `PROT_NONE` reserve of `2*B`, then
the same `memfd` `MAP_FIXED` over both halves. A record running off the physical
end reappears at the start, so it stays contiguous *in virtual memory*:

```c
memcpy(base + (idx & mask),     &len, 4);   /* one memcpy, no wrap branch */
memcpy(base + (idx & mask) + 4, data, len);
```

The reserve-then-overmap order is load-bearing: two independent `mmap` calls can
land anywhere, and the halves must be adjacent.

**`reclen <= capacity` is the premise the whole scheme rests on.** It is why
`create` raises the capacity to fit one `max_msg_size` record, on top of the page
floor and the caller's request. The furthest byte any record touches is
`(B-1) + reclen - 1 < 2B`, so the span always lands inside the mapping. Drop
that raise and the buffer silently becomes unusable — a `max_msg_size` push is
refused forever, because the fullness check can never be satisfied.

**Records are 4-byte aligned (`REC_ALIGN`) so the `u32` header is never torn.**
A pleasant consequence: since the capacity is also a multiple of 4, a header
*never splits* across the end — only bodies do. The `straddle` test's `gap == 4`
case pins this down.

### What is different from `ringbuf.c`, and why

- **No `read_idx` CAS.** Both writers hold the single boundary spinlock across
  their whole operation, so a plain store is already single-valued.
  `received + dropped == pushed` still holds, by a different argument: every
  advance happens under the lock and moves `read_idx` by exactly one record's
  `reclen`, so each record is retired exactly once.
- **One boundary lock replaces the per-slot array.** Both sides only ever touch
  the record *at* `read_idx` — the consumer to take it, the producer to evict it
  — so there is exactly one contested record at any moment. A spinlock, not a
  mutex, so `push` stays non-blocking and signal-safe like `ringbuf.c`; both
  sides can still fail transiently, so **every caller must retry**.
- **Eviction is a loop.** Variable sizes mean one push may have to retire
  several records to free enough bytes. The fixed-size variants evict at most
  one slot per push.
- **`pop` re-reads `read_idx` after acquiring the lock.** Same trap as the mutex
  variant, but worse here: a stale index would make `pop` read the **length
  header** of a record the producer already evicted and refilled, so a bogus
  `reclen` would drive the `memcpy`. Verified by breaking it — `overwrite_stress`
  wedges, while the single-threaded eviction tests still pass and cannot catch
  it.
- **The ordering rule is unchanged** from `ringbuf_mutex.c`: every non-relaxed
  ordering has an unlocked access on at least one end; if both ends lock,
  relaxed is correct. The acquire/release pair on the unlocked fast path — not
  the lock — is what prevents the record-data race. Relaxing `pop`'s `read_idx`
  store makes tsan report a `__tsan_memcpy` race between `store_record` and the
  consumer's `read_record_len`/body copy, *not* a race on `read_idx`.

### tsan is a weaker gate here

ThreadSanitizer keys shadow memory by **virtual address**, so the same physical
byte seen at `base + o` and `base + o + B` looks like two unrelated locations.
That can only ever *hide* a race on a record which straddles the end, never
invent one — so a clean `ringbuf_vslot_tsan` run is weaker evidence than for the
other two variants. The `straddle` and `wrap_*` cases, which check sequence
numbers and lengths rather than relying on instrumentation, are the real gate.
Do not take a green tsan as proof on its own.

### Counter overflow

Survives, and `SEED_BASE` is **`SIZE_MAX - 3`** rather than `ringbuf.c`'s
`SIZE_MAX - 2`, because the seed must be 4-aligned like every record start.
`2^64` is a multiple of both `REC_ALIGN` and the capacity (a power of two
`>= 4096`), so alignment and `& mask` congruence both survive the wrap. Seeding
there puts the first record's header in the final 4 bytes of the ring and its
body at offset 0 *through the mirror*, so the `wrap_*` cases exercise the mirror
and the overflow at once.

`test_ringbuf_vslot.c` runs **ten** cases: the seven that mirror `test_ringbuf.c`
plus `straddle`, `truncate`, and `pop_cap`. Message lengths are derived from the
sequence number (`size_for_seq`), so the consumer recomputes the length it
*should* have received and a **mis-sized record fails the test even when its
bytes are intact** — the failure mode specific to this variant.

There is deliberately **no benchmark target** for it: a fair comparison against
the fixed-size variants needs a size distribution and a bytes/s axis, which is
its own piece of work rather than an extra row in the existing table.

**Counters are monotonic and mapped to slots with `& mask`, so they wrap at
`SIZE_MAX`.** Emptiness is therefore a *signed* comparison,
`(ptrdiff_t)(cached_write - read_idx) <= 0`, never `read_idx >= cached_write` —
the absolute form reads "empty forever" once `write_idx` has wrapped and
`read_idx` has not. Fullness compares against `capacity` the same way. Any edit
to this arithmetic needs the wrap tests below.

Capacity is rounded up to a power of two (minimum 2) so the mask works.
`write_idx` and `read_idx` are `_Alignas(64)` on separate cache lines, each with
a private cache of the other's counter (`cached_read` / `cached_write`) to avoid
touching the peer's line on every operation.

**One allocation per buffer** (`ringbuf` and `ringbuf_mutex`; `ringbuf_vslot`
needs two, since a mirrored mapping cannot live inside the control block's
allocation). The slot data and the optional lock array live in
a `_Alignas(64) char tail[]` flexible array member, so `ringbuf_create` is a
single `posix_memalign` and `ringbuf_destroy` a single `free` — there is no
separate `buffer` pointer to chase. The lock array starts at `tail +
align_up(capacity * slot_size, _Alignof(atomic_int))`, since `slot_size` carries
no alignment guarantee of its own; `slot_lock` caches that address rather than
recomputing it on the hot path.

Because `slot_size` and `min_capacity` are caller-supplied, every step of the
size computation is overflow-checked (`add_checked`, `align_up`, and the two
division guards). An unchecked `capacity * slot_size` would wrap and
under-allocate, turning every subsequent slot write into a heap overflow.

### Test hooks

`RINGBUF_TEST_HOOKS` exposes `ringbuf_test_seed_counters()`, which parks both
counters near `SIZE_MAX` so the four `wrap_*` tests can cross the overflow
boundary — otherwise unreachable without 2^64 pushes. It is test-only and never
shipped.

The define must reach `ringbuf.c`, not just the test file, so **the test targets
compile `ringbuf.c` directly rather than linking the `ringbuf` library.** Linking
the library instead would silently skip the four wrap tests (`main` `#ifdef`s
them out). Keep this in mind when touching `CMakeLists.txt`. The mutex variant
has the identical arrangement with `ringbuf_mutex_test_seed_counters()`, and so
does `ringbuf_vslot` with `ringbuf_vslot_test_seed_counters()` — note that one
rounds its argument down to `REC_ALIGN`. `ringbuf_mpsc` and `ringbuf_zcopy`
likewise, each with **five** cases behind the define, and they are the two hooks
that do more than store counters: both must also re-stamp every slot, with the
same `offset = (i - (start & mask)) & mask` derivation. Omitting that does not
fail a check in `ringbuf_mpsc` — it **hangs**; in `ringbuf_zcopy` the test's
`MAX_SPIN` bound turns it into a failure of all five `wrap_*` cases instead.
`test_ringbuf_zcopy_mpsc.c` uses the same `ringbuf_zcopy` hook for a sixth
`wrap_*` case of its own, which is why it also compiles the library source
directly.

## Style

Linux kernel style with two deliberate deviations: 4-space indents and spaces
instead of tabs. Enforced by `.clang-format` (kernel braces, 80 columns, system
headers before project headers via `IncludeBlocks: Regroup`).

**No typedefs for structs** — declare `struct foo` and spell it out at use
sites. clang-format cannot enforce this, so it needs attention in review. The
only `_t` names should be standard-library ones (`size_t`, `pthread_t`, …).

### Function comments

**Every function definition and public declaration carries a kernel-doc
block**, in the kernel's format:

```c
/**
 * ringbuf_push - copy one message into the buffer
 * @rb: the buffer
 * @data: slot_size bytes to copy in
 *
 * Producer side only. Free-form body for the things a caller cannot infer
 * from the signature.
 *
 * Return: true if the message was stored; false if the buffer is full.
 */
```

`/**` opener, `name - one-line summary` first, one `@param:` line per
parameter in declaration order, then the optional body, then `Return:` last
for anything non-void. Continuation lines in a `@param:` entry are indented
with a tab, which is the one place a tab is correct in this tree. Statics get
the same treatment — `next_pow2`, the pthread entry points, the benchmark
helpers. For thread entry points that always return `NULL` and report through
their args, say so in `Return:` rather than omitting it.

The contract lives in the **header**; the definition's block is shorter and
points back with "see ringbuf.h", covering only what is specific to the
implementation (which paths lock, which stores are relaxed and why). Don't
duplicate the full contract in both places — it drifts.

clang-format does not enforce any of this and will not reflow these blocks,
so it is a review item like the typedef rule. Internal commentary that
explains a *line* rather than a function stays an ordinary `/* */` comment
inside the body; kernel-doc is for the interface.

```bash
nix shell nixpkgs#clang-tools --command clang-format -i *.c *.h
```

## clangd

`CMAKE_EXPORT_COMPILE_COMMANDS` is set **before** `project()` so it also covers
the compiler checks `project()` runs. A root `compile_commands.json` symlink
points into the build dir and is gitignored (it dangles on a fresh clone;
recreate with `ln -s build/compile_commands.json compile_commands.json`).

`CMakeLists.txt` replays `CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES` as `-isystem`
flags. This is load-bearing on Nix: the compiler is a wrapper script that injects
header paths via `NIX_CFLAGS_COMPILE` at invocation time, those flags never reach
`compile_commands.json`, clangd runs the bare driver without that environment,
and there is no `/usr/include` to fall back on — so without this every system
header goes unresolved. The `SHELL:` prefix keeps each `-isystem <path>` pair
together; without it CMake de-duplicates the repeated bare `-isystem` token and
orphans the second path.
