cmake_minimum_required(VERSION 3.16)

# Emit build/compile_commands.json for clangd. Set before project() so it
# covers every target, including the compiler checks project() itself runs.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(ringbuf LANGUAGES C)

# The Makefile this replaced defaulted to -O2 -g.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(RINGBUF_BUILD_TESTS "Build the ringbuf test binary" ON)
option(RINGBUF_BUILD_TSAN "Also build a ThreadSanitizer test binary" OFF)
option(RINGBUF_BUILD_BENCH "Also build the benchmark binary" OFF)

find_package(Threads REQUIRED)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set(RINGBUF_WARNINGS -Wall -Wextra)
else()
    set(RINGBUF_WARNINGS)
endif()

# Bake the compiler's own system include dirs into compile_commands.json.
#
# On Nix, CMAKE_C_COMPILER is a wrapper script that injects the paths to
# the freestanding headers (stddef.h) and the libc headers (stdio.h) via
# NIX_CFLAGS_COMPILE at invocation time. Those flags never reach
# compile_commands.json, and clangd runs the bare clang driver without
# the wrapper's environment -- so it resolves neither. There is no
# /usr/include to fall back on, so every system header goes missing.
#
# CMake already probes the real search path during compiler detection, so
# reuse its answer rather than re-deriving it. Harmless elsewhere: on a
# normal toolchain these are the dirs clangd would have found anyway.
#
# The SHELL: prefix keeps each flag/path pair together. Without it CMake
# de-duplicates the repeated bare "-isystem" token and the second path
# onward is emitted with no flag at all.
foreach(dir IN LISTS CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES)
    add_compile_options("SHELL:-isystem ${dir}")
endforeach()

add_library(ringbuf ringbuf.c ringbuf.h)
target_include_directories(ringbuf PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(ringbuf PRIVATE ${RINGBUF_WARNINGS})
target_link_libraries(ringbuf PUBLIC Threads::Threads)

# Mutex variant: same structure and same external SPSC contract, but
# overwrite mode is reconciled with one internal mutex instead of the
# per-slot spinlocks and read_idx CAS. Independent target with prefixed
# symbols, so both can be linked into one binary.
add_library(ringbuf_mutex ringbuf_mutex.c ringbuf_mutex.h)
target_include_directories(ringbuf_mutex PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(ringbuf_mutex PRIVATE ${RINGBUF_WARNINGS})
target_link_libraries(ringbuf_mutex PUBLIC Threads::Threads)

# Multi-producer variant: lifts the single-producer restriction, which is a
# different axis than ringbuf_mutex (synchronization of the same contract).
# Producers claim a slot with a ticket CAS on write_idx and publish via a
# per-slot stamp. Plain C11 + POSIX, so unlike ringbuf_vslot it is not
# gated on Linux.
add_library(ringbuf_mpsc ringbuf_mpsc.c ringbuf_mpsc.h)
target_include_directories(ringbuf_mpsc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(ringbuf_mpsc PRIVATE ${RINGBUF_WARNINGS})
target_link_libraries(ringbuf_mpsc PUBLIC Threads::Threads)

# Reserve/submit variant: ringbuf_mutex with push split in two, so the
# caller fills the slot in place and several fills can overlap. Still SPSC.
# Out-of-order submit means write_idx can no longer double as the readiness
# marker -- stored slots stop being a prefix of the ring -- so publication
# moves to a per-slot stamp. Plain C11 + POSIX, so not Linux-gated.
add_library(ringbuf_zcopy ringbuf_zcopy.c ringbuf_zcopy.h)
target_include_directories(ringbuf_zcopy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(ringbuf_zcopy PRIVATE ${RINGBUF_WARNINGS})
target_link_libraries(ringbuf_zcopy PUBLIC Threads::Threads)

# There is deliberately NO ringbuf_zcopy_mpsc library target. Several
# producers are supported by ringbuf_zcopy itself, provided the caller
# serializes reserve() with a mutex it owns -- that file's single-producer
# reasoning needs the reserves serialized, not same-threaded. A separate
# implementation was written first and came out token-for-token identical to
# ringbuf_zcopy.c, so it was deleted rather than maintained in parallel. What
# remains is test_ringbuf_zcopy_mpsc.c, which links the same library and is
# the only place the multi-producer discipline is exercised.

# Variable-length variant: byte-packed, length-prefixed records instead of
# fixed-size slots, with the ring mapped twice back to back so a record
# spanning the physical end stays contiguous.
#
# Linux-only, and the one target here that is: the mirror needs
# memfd_create(2). Skipped quietly rather than FATAL_ERROR, unlike
# RINGBUF_BUILD_BENCH below -- that one is opt-in, so failing loudly is
# fine, whereas this would break a default configure on a non-Linux host.
set(RINGBUF_HAVE_VAR OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(RINGBUF_HAVE_VAR ON)

    add_library(ringbuf_vslot ringbuf_vslot.c ringbuf_vslot.h)
    target_include_directories(ringbuf_vslot PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(ringbuf_vslot PRIVATE ${RINGBUF_WARNINGS})
    target_link_libraries(ringbuf_vslot PUBLIC Threads::Threads)
else()
    message(STATUS
        "ringbuf_vslot skipped: needs memfd_create (Linux); "
        "got ${CMAKE_SYSTEM_NAME}.")
endif()

if(RINGBUF_BUILD_TESTS)
    enable_testing()

    # Exposes ringbuf_test_seed_counters() so the test binary can exercise the
    # SIZE_MAX counter-overflow boundary. Test builds only, never shipped code.
    #
    # The define has to reach ringbuf.c too, so the test binaries compile the
    # library sources themselves rather than linking the `ringbuf` target.
    add_executable(test_ringbuf ringbuf.c test_ringbuf.c)
    target_compile_definitions(test_ringbuf PRIVATE RINGBUF_TEST_HOOKS)
    target_compile_options(test_ringbuf PRIVATE ${RINGBUF_WARNINGS})
    target_link_libraries(test_ringbuf PRIVATE Threads::Threads)
    add_test(NAME ringbuf COMMAND test_ringbuf)

    # Same deal for the mutex variant: compile ringbuf_mutex.c directly, do
    # not link the ringbuf_mutex target, or the four wrap tests vanish.
    add_executable(test_ringbuf_mutex ringbuf_mutex.c test_ringbuf_mutex.c)
    target_compile_definitions(test_ringbuf_mutex PRIVATE RINGBUF_TEST_HOOKS)
    target_compile_options(test_ringbuf_mutex PRIVATE ${RINGBUF_WARNINGS})
    target_link_libraries(test_ringbuf_mutex PRIVATE Threads::Threads)
    add_test(NAME ringbuf_mutex COMMAND test_ringbuf_mutex)

    # Same deal for the MPSC variant: compile ringbuf_mpsc.c directly, or
    # the five counter-overflow tests vanish.
    add_executable(test_ringbuf_mpsc ringbuf_mpsc.c test_ringbuf_mpsc.c)
    target_compile_definitions(test_ringbuf_mpsc PRIVATE RINGBUF_TEST_HOOKS)
    target_compile_options(test_ringbuf_mpsc PRIVATE ${RINGBUF_WARNINGS})
    target_link_libraries(test_ringbuf_mpsc PRIVATE Threads::Threads)
    add_test(NAME ringbuf_mpsc COMMAND test_ringbuf_mpsc)

    # Same deal for the reserve/submit variant: compile ringbuf_zcopy.c
    # directly, or the five counter-overflow tests vanish.
    add_executable(test_ringbuf_zcopy ringbuf_zcopy.c test_ringbuf_zcopy.c)
    target_compile_definitions(test_ringbuf_zcopy PRIVATE RINGBUF_TEST_HOOKS)
    target_compile_options(test_ringbuf_zcopy PRIVATE ${RINGBUF_WARNINGS})
    target_link_libraries(test_ringbuf_zcopy PRIVATE Threads::Threads)
    add_test(NAME ringbuf_zcopy COMMAND test_ringbuf_zcopy)

    # The multi-producer cases for the SAME library: several producers are
    # legal when the caller serializes reserve() with its own mutex, which no
    # single-producer test file can demonstrate. Compiles ringbuf_zcopy.c
    # directly like the target above -- it is a second test binary over one
    # library, not a second implementation -- and needs the define for its own
    # counter-overflow case.
    add_executable(test_ringbuf_zcopy_mpsc
        ringbuf_zcopy.c test_ringbuf_zcopy_mpsc.c)
    target_compile_definitions(test_ringbuf_zcopy_mpsc PRIVATE
        RINGBUF_TEST_HOOKS)
    target_compile_options(test_ringbuf_zcopy_mpsc PRIVATE ${RINGBUF_WARNINGS})
    target_link_libraries(test_ringbuf_zcopy_mpsc PRIVATE Threads::Threads)
    add_test(NAME ringbuf_zcopy_mpsc COMMAND test_ringbuf_zcopy_mpsc)

    # And again for the variable-length variant, when it is available.
    if(RINGBUF_HAVE_VAR)
        add_executable(test_ringbuf_vslot ringbuf_vslot.c test_ringbuf_vslot.c)
        target_compile_definitions(test_ringbuf_vslot PRIVATE RINGBUF_TEST_HOOKS)
        target_compile_options(test_ringbuf_vslot PRIVATE ${RINGBUF_WARNINGS})
        target_link_libraries(test_ringbuf_vslot PRIVATE Threads::Threads)
        add_test(NAME ringbuf_vslot COMMAND test_ringbuf_vslot)
    endif()

    if(RINGBUF_BUILD_TSAN)
        if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
            message(FATAL_ERROR
                "RINGBUF_BUILD_TSAN requires GCC or Clang "
                "(got ${CMAKE_C_COMPILER_ID}).")
        endif()

        add_executable(test_ringbuf_tsan ringbuf.c test_ringbuf.c)
        target_compile_definitions(test_ringbuf_tsan PRIVATE RINGBUF_TEST_HOOKS)
        # -O1 keeps the sanitized build fast enough to run while preserving
        # readable reports; overrides whatever the build type would supply.
        target_compile_options(test_ringbuf_tsan PRIVATE
            ${RINGBUF_WARNINGS} -O1 -g -fsanitize=thread)
        target_link_options(test_ringbuf_tsan PRIVATE -fsanitize=thread)
        target_link_libraries(test_ringbuf_tsan PRIVATE Threads::Threads)
        add_test(NAME ringbuf_tsan COMMAND test_ringbuf_tsan)

        # The real gate on the mutex variant: it is what proves the unlocked
        # fast push path does not race a concurrent locked pop.
        add_executable(test_ringbuf_mutex_tsan
            ringbuf_mutex.c test_ringbuf_mutex.c)
        target_compile_definitions(test_ringbuf_mutex_tsan PRIVATE
            RINGBUF_TEST_HOOKS)
        target_compile_options(test_ringbuf_mutex_tsan PRIVATE
            ${RINGBUF_WARNINGS} -O1 -g -fsanitize=thread)
        target_link_options(test_ringbuf_mutex_tsan PRIVATE -fsanitize=thread)
        target_link_libraries(test_ringbuf_mutex_tsan PRIVATE
            Threads::Threads)
        add_test(NAME ringbuf_mutex_tsan COMMAND test_ringbuf_mutex_tsan)

        # The real gate on the MPSC variant. It has more concurrent writers
        # than anything else here -- N producers on write_idx and the
        # per-slot stamps -- and the stamp acquire/release pair is the only
        # thing keeping a producer's memcpy off a slot the consumer is still
        # reading. Nothing in the logic tests can observe that directly.
        add_executable(test_ringbuf_mpsc_tsan
            ringbuf_mpsc.c test_ringbuf_mpsc.c)
        target_compile_definitions(test_ringbuf_mpsc_tsan PRIVATE
            RINGBUF_TEST_HOOKS)
        target_compile_options(test_ringbuf_mpsc_tsan PRIVATE
            ${RINGBUF_WARNINGS} -O1 -g -fsanitize=thread)
        target_link_options(test_ringbuf_mpsc_tsan PRIVATE -fsanitize=thread)
        target_link_libraries(test_ringbuf_mpsc_tsan PRIVATE
            Threads::Threads)
        add_test(NAME ringbuf_mpsc_tsan COMMAND test_ringbuf_mpsc_tsan)

        # The real gate on the reserve/submit variant, and the one whose
        # ordering is hardest to get right: the window between reserve and
        # submit spans threads, so correctness rests on a three-edge chain
        # (reserve's acquire, the caller's own handoff to the filler, and
        # submit's release). The parallel_fill cases are what drive it, and
        # nothing in their logic checks can observe a missing release
        # directly.
        add_executable(test_ringbuf_zcopy_tsan
            ringbuf_zcopy.c test_ringbuf_zcopy.c)
        target_compile_definitions(test_ringbuf_zcopy_tsan PRIVATE
            RINGBUF_TEST_HOOKS)
        target_compile_options(test_ringbuf_zcopy_tsan PRIVATE
            ${RINGBUF_WARNINGS} -O1 -g -fsanitize=thread)
        target_link_options(test_ringbuf_zcopy_tsan PRIVATE -fsanitize=thread)
        target_link_libraries(test_ringbuf_zcopy_tsan PRIVATE
            Threads::Threads)
        add_test(NAME ringbuf_zcopy_tsan COMMAND test_ringbuf_zcopy_tsan)

        # The real gate on the multi-producer discipline, and the one place
        # its central claim can actually be checked. ringbuf_zcopy.c's
        # accesses to write_idx are relaxed at both ends, which is correct
        # under several producers ONLY because the caller's mutex supplies the
        # release/acquire edge between consecutive reserves -- nothing in the
        # library provides it. No logic test can observe that edge; tsan can,
        # and reports a race in the caller's fill (two producers handed the
        # same slot) if the mutex stops covering the claim. Verified by
        # removing it. The mpsc_* cases, with four producers handing write_idx
        # between them, are what drive it.
        add_executable(test_ringbuf_zcopy_mpsc_tsan
            ringbuf_zcopy.c test_ringbuf_zcopy_mpsc.c)
        target_compile_definitions(test_ringbuf_zcopy_mpsc_tsan PRIVATE
            RINGBUF_TEST_HOOKS)
        target_compile_options(test_ringbuf_zcopy_mpsc_tsan PRIVATE
            ${RINGBUF_WARNINGS} -O1 -g -fsanitize=thread)
        target_link_options(test_ringbuf_zcopy_mpsc_tsan PRIVATE
            -fsanitize=thread)
        target_link_libraries(test_ringbuf_zcopy_mpsc_tsan PRIVATE
            Threads::Threads)
        add_test(NAME ringbuf_zcopy_mpsc_tsan
            COMMAND test_ringbuf_zcopy_mpsc_tsan)

        # Weaker evidence than the other five tsan targets, deliberately
        # kept anyway. tsan keys shadow memory by virtual address, so the
        # two views of the mirrored mapping look like unrelated locations
        # and a race on a record spanning the ring end can be hidden. It
        # still covers every access that does not straddle, which is most
        # of them; the straddle and wrap_* cases are the real gate. See
        # the note on map_mirrored in ringbuf_vslot.c.
        if(RINGBUF_HAVE_VAR)
            add_executable(test_ringbuf_vslot_tsan
                ringbuf_vslot.c test_ringbuf_vslot.c)
            target_compile_definitions(test_ringbuf_vslot_tsan PRIVATE
                RINGBUF_TEST_HOOKS)
            target_compile_options(test_ringbuf_vslot_tsan PRIVATE
                ${RINGBUF_WARNINGS} -O1 -g -fsanitize=thread)
            target_link_options(test_ringbuf_vslot_tsan PRIVATE
                -fsanitize=thread)
            target_link_libraries(test_ringbuf_vslot_tsan PRIVATE
                Threads::Threads)
            add_test(NAME ringbuf_vslot_tsan COMMAND test_ringbuf_vslot_tsan)
        endif()
    endif()
endif()

# Benchmark. Opt-in and OFF by default: it runs for tens of seconds and is a
# measurement, not a pass/fail check, so it is deliberately NOT an add_test
# entry -- `ctest` stays fast and its results stay meaningful.
#
#   cmake -B build -DRINGBUF_BUILD_BENCH=ON && cmake --build build -j
#   ./build/bench_ringbuf
#
# Compiles ringbuf.c directly for symmetry with the test targets. Always -O2
# regardless of build type: benchmarking a -O0 or -Og build measures nothing
# useful, and a stray Debug configure would silently produce garbage numbers.
if(RINGBUF_BUILD_BENCH)
    # Pins threads to fixed CPUs via pthread_setaffinity_np, which is a glibc
    # extension -- hence the _GNU_SOURCE the source defines for itself. Fail
    # at configure time rather than with a confusing compile error.
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR
            "RINGBUF_BUILD_BENCH needs pthread_setaffinity_np (Linux); "
            "got ${CMAKE_SYSTEM_NAME}.")
    endif()

    add_executable(bench_ringbuf bench_ringbuf.c ringbuf.c)
    target_compile_options(bench_ringbuf PRIVATE ${RINGBUF_WARNINGS} -O2 -g)
    target_link_libraries(bench_ringbuf PRIVATE Threads::Threads)

    # Head-to-head: the two real implementations against each other, with CPU
    # consumption alongside throughput. bench_ringbuf compares ringbuf to an
    # internal toy mutex ring; this one links BOTH variants, which is why the
    # prefixed symbols exist.
    add_executable(bench_ringbuf_vs_ringbuf_mutex
        bench_ringbuf_vs_ringbuf_mutex.c ringbuf.c ringbuf_mutex.c)
    target_compile_options(bench_ringbuf_vs_ringbuf_mutex PRIVATE
        ${RINGBUF_WARNINGS} -O2 -g)
    target_link_libraries(bench_ringbuf_vs_ringbuf_mutex PRIVATE
        Threads::Threads)

    # Multi-producer fan-in: ringbuf_mpsc against ringbuf_mutex made usable
    # from N producers by a caller-held push mutex. Unlike the comparison
    # above, the two sides do not share a contract -- the external mutex is
    # what a caller would have to write, and pricing it is the point.
    add_executable(bench_ringbuf_mpsc_vs_ringbuf_mutex
        bench_ringbuf_mpsc_vs_ringbuf_mutex.c ringbuf_mpsc.c ringbuf_mutex.c)
    target_compile_options(bench_ringbuf_mpsc_vs_ringbuf_mutex PRIVATE
        ${RINGBUF_WARNINGS} -O2 -g)
    target_link_libraries(bench_ringbuf_mpsc_vs_ringbuf_mutex PRIVATE
        Threads::Threads)
endif()
