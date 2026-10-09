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

`build/` already holds a **Unix Makefiles** cache. `-G Ninja` will refuse to
reconfigure over it; `rm -rf build` first if you want to switch generators.

### Running a single test case

Not supported. `main(void)` in `test_ringbuf.c` takes no arguments and runs all
7 cases unconditionally; there is no filter flag. To isolate one, comment out
the others in `main` or add arg parsing.

The two stress tests push 2,000,000 messages each and dominate the ~0.2 s
runtime; the tsan build takes ~2 s.

## Architecture

Single-producer / single-consumer lock-free ring buffer. Three source files:
`ringbuf.{c,h}` (the library) and `test_ringbuf.c`. CMake builds a `ringbuf`
static library plus the test binaries.

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

### Test hooks

`RINGBUF_TEST_HOOKS` exposes `ringbuf_test_seed_counters()`, which parks both
counters near `SIZE_MAX` so the four `wrap_*` tests can cross the overflow
boundary — otherwise unreachable without 2^64 pushes. It is test-only and never
shipped.

The define must reach `ringbuf.c`, not just the test file, so **the test targets
compile `ringbuf.c` directly rather than linking the `ringbuf` library.** Linking
the library instead would silently skip the four wrap tests (`main` `#ifdef`s
them out). Keep this in mind when touching `CMakeLists.txt`.

## Style

Linux kernel style with two deliberate deviations: 4-space indents and spaces
instead of tabs. Enforced by `.clang-format` (kernel braces, 80 columns, system
headers before project headers via `IncludeBlocks: Regroup`).

**No typedefs for structs** — declare `struct foo` and spell it out at use
sites. clang-format cannot enforce this, so it needs attention in review. The
only `_t` names should be standard-library ones (`size_t`, `pthread_t`, …).

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
