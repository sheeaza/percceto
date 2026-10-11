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

With the mutex and variable-length variants there are now **six** entries —
`ringbuf`, `ringbuf_mutex`, `ringbuf_vslot`, and the three opt-in `*_tsan` ones —
so `-R ringbuf` matches all six and the anchored form matters more than before.

`build/` already holds a **Unix Makefiles** cache. `-G Ninja` will refuse to
reconfigure over it; `rm -rf build` first if you want to switch generators.

`ringbuf_vslot` and its test targets are **gated on Linux** (`memfd_create`), with
a `message(STATUS)` skip rather than a `FATAL_ERROR` — unlike the benchmarks,
which are opt-in and so can fail loudly. On a non-Linux host the build succeeds
with four CTest entries instead of six.

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
no arguments and runs all 7 cases unconditionally (`test_ringbuf_vslot.c` runs
all 10); there is no filter flag. To isolate one, comment out the others in
`main` or add arg parsing.

The two stress tests push 2,000,000 messages each and dominate the ~0.2 s
runtime; the tsan build takes ~2 s.

## Two implementations

`ringbuf.{c,h}` is the lock-free original. `ringbuf_mutex.{c,h}` is a sibling
with the **same data structure and the same external SPSC contract**, differing
only in how overwrite mode reconciles its two writers to `read_idx`. Symbols are
prefixed (`ringbuf_mutex_push`, `struct ringbuf_mutex`, …) so both can link into
one binary.

(There is also a third, `ringbuf_vslot.{c,h}`, which changes the *storage model*
rather than the synchronization — see "The variable-length variant" below.)

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

## Architecture

Single-producer / single-consumer lock-free ring buffer. Three implementations:
`ringbuf.{c,h}` and `ringbuf_mutex.{c,h}` (interchangeable, fixed-size slots —
see "Two implementations" above), plus `ringbuf_vslot.{c,h}` (variable-length
messages — see "The variable-length variant"). `test_ringbuf.c` and
`test_ringbuf_mutex.c` mirror each other case for case, `test_ringbuf_vslot.c`
adds three cases of its own, and two benchmarks sit alongside
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

## The variable-length variant

`ringbuf_vslot.{c,h}` is the third implementation, and the only one that changes
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
rounds its argument down to `REC_ALIGN`.

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
