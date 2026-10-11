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

With the mutex variant there are now **four** entries — `ringbuf`,
`ringbuf_mutex`, and the two opt-in `*_tsan` ones — so `-R ringbuf` matches all
four and the anchored form matters more than before.

`build/` already holds a **Unix Makefiles** cache. `-G Ninja` will refuse to
reconfigure over it; `rm -rf build` first if you want to switch generators.

### Benchmark

Opt-in, and deliberately **not** CTest entries — they are measurements, not
pass/fail checks, and they run for tens of seconds to minutes:

```bash
cmake -B build -DRINGBUF_BUILD_BENCH=ON && cmake --build build -j
./build/bench_ringbuf                      # ringbuf vs an internal toy mutex ring
./build/bench_ringbuf_vs_ringbuf_mutex     # the two real variants (~4 min)
```

Both are always compiled `-O2` regardless of `CMAKE_BUILD_TYPE`, so a stray
Debug configure cannot silently produce meaningless numbers. Linux-only: they
pin the producer and consumer to fixed CPUs with `pthread_setaffinity_np`,
without which a mid-run migration shows up as a throughput cliff unrelated to
the code. CMake fails at configure time on other platforms.

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

### Running a single test case

Not supported. `main(void)` in `test_ringbuf.c` and `test_ringbuf_mutex.c` takes
no arguments and runs all 7 cases unconditionally; there is no filter flag. To
isolate one, comment out the others in `main` or add arg parsing.

The two stress tests push 2,000,000 messages each and dominate the ~0.2 s
runtime; the tsan build takes ~2 s.

## Two implementations

`ringbuf.{c,h}` is the lock-free original. `ringbuf_mutex.{c,h}` is a sibling
with the **same data structure and the same external SPSC contract**, differing
only in how overwrite mode reconciles its two writers to `read_idx`. Symbols are
prefixed (`ringbuf_mutex_push`, `struct ringbuf_mutex`, …) so both can link into
one binary.

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
the single allocation applies to both.

## Architecture

Single-producer / single-consumer lock-free ring buffer. Two interchangeable
implementations (see "Two implementations" above): `ringbuf.{c,h}` plus
`ringbuf_mutex.{c,h}`, with `test_ringbuf.c` and `test_ringbuf_mutex.c`
mirroring each other case for case, and two benchmarks alongside
(`bench_ringbuf.c`, `bench_ringbuf_vs_ringbuf_mutex.c`). CMake builds a static
library per variant plus the test binaries.

Exactly one thread may call `ringbuf_push`, exactly one may call `ringbuf_pop`.
More than one on either side is UB — this is the premise the whole design rests
on, not a caveat.

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

**One allocation per buffer.** The slot data and the optional lock array live in
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
has the identical arrangement with `ringbuf_mutex_test_seed_counters()`.

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
