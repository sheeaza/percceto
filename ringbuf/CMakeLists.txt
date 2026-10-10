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
    endif()
endif()
