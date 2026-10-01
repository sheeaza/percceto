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
  src/pftrace.c
)
target_include_directories(pftrace PUBLIC include)
target_link_libraries(pftrace PUBLIC Threads::Threads)
# strdup, clock_gettime and CLOCK_BOOTTIME are POSIX/BSD additions that glibc
# hides under the strict ISO C definitions. -std=gnu11 happens to define this
# for us, so the build works without it, but saying so explicitly keeps the
# sources compilable under a strict -std=c11 too. PRIVATE: the public header
# is self-contained and needs no feature macro from consumers.
target_compile_definitions(pftrace PRIVATE _DEFAULT_SOURCE)
set_target_properties(pftrace PROPERTIES
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

add_executable(example_basic examples/example_basic.c)
target_link_libraries(example_basic PRIVATE pftrace)
# usleep, as above.
target_compile_definitions(example_basic PRIVATE _DEFAULT_SOURCE)
