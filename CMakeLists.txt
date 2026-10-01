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

# Keep every artifact at the top of the build dir even though the targets
# are now declared in subdirectories. Without this CMake mirrors the source
# tree (build/tests/test_ring, build/tools/dump_trace), which splits the
# build files -- the point of the subdirectories -- but also moves every
# binary, breaking existing invocations and tools/run_checks.sh for no gain.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Must precede add_subdirectory(tests), or the add_test() calls there have
# no test harness to register against.
enable_testing()

# Each directory owns the targets built from its own sources. Build the
# library first: everything downstream links it.
add_subdirectory(src)
add_subdirectory(examples)
add_subdirectory(tools)
add_subdirectory(tests)
