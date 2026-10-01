cmake_minimum_required(VERSION 3.10)

# Emit build/compile_commands.json for clangd. Set before project() so it
# covers every target, including the compiler checks project() itself runs.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(pftrace C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

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

find_package(Threads REQUIRED)

add_library(pftrace STATIC
  src/pb_writer.c
  src/pf_ring.c
  src/pftrace.c
)
target_include_directories(pftrace PUBLIC include)
target_link_libraries(pftrace PUBLIC Threads::Threads)
# memfd_create (the ring's double mapping) is glibc's, not ISO C's, and
# unlike clock_gettime/CLOCK_BOOTTIME it is not exposed by _DEFAULT_SOURCE --
# only _GNU_SOURCE declares it. Without this the build fails on an implicit
# declaration. PRIVATE: the public header is self-contained and needs no
# feature macro from consumers.
target_compile_definitions(pftrace PRIVATE _GNU_SOURCE)
set_target_properties(pftrace PROPERTIES
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

add_executable(example_basic examples/example_basic.c)
target_link_libraries(example_basic PRIVATE pftrace)
# usleep, as above.
target_compile_definitions(example_basic PRIVATE _DEFAULT_SOURCE)

# Validates a trace against the protobuf wire format without needing the
# perfetto Python package, which is not installable everywhere. Doubles as
# the CI gate; verify_trace.py and check_schema.py are the stronger checks
# where a real generated schema is available.
#
# Deliberately does not link pftrace: decoding with the encoder's own
# constants would let a wrong field number agree with itself.
add_executable(dump_trace tools/dump_trace.c)

# Ring mechanics in isolation: compiles pf_ring.c directly rather than
# linking pftrace, so the ring can be tested without protobuf, files or any
# of the library's state. That is also why it needs src/ on the include
# path -- pf_ring.h is private to the implementation.
add_executable(test_ring tests/test_ring.c src/pf_ring.c)
target_include_directories(test_ring PRIVATE src)
target_link_libraries(test_ring PRIVATE Threads::Threads)
target_compile_definitions(test_ring PRIVATE _GNU_SOURCE)

# Drives enough events through a small ring to force many wraps, which is
# where snapshot windowing and continuous gaplessness actually get tested.
# memmem, used to confirm which events survived in a window, needs
# _GNU_SOURCE.
add_executable(test_stress tests/test_stress.c)
target_link_libraries(test_stress PRIVATE pftrace Threads::Threads)
target_compile_definitions(test_stress PRIVATE _GNU_SOURCE)

# Interposes malloc/calloc/realloc to prove the hot path allocates nothing
# once the trace is open -- requirement 1. Needs dl for RTLD_NEXT.
add_executable(test_noalloc tests/test_noalloc.c)
target_link_libraries(test_noalloc
  PRIVATE pftrace Threads::Threads ${CMAKE_DL_LIBS}
)

# Not a test: reports producer vs drain cost per event, to show where the
# hot path actually stands and at what rate continuous mode starts dropping.
add_executable(bench tests/bench.c)
target_link_libraries(bench PRIVATE pftrace Threads::Threads)

enable_testing()
add_test(NAME ring COMMAND test_ring)
add_test(NAME stress COMMAND test_stress)
add_test(NAME noalloc COMMAND test_noalloc)
