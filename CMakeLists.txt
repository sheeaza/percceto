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

# Some clangd builds (notably minimal Nix packages) ship no bundled
# resource-dir headers and mis-detect the compiler's private include
# dir on their own, dropping freestanding headers like stddef.h. Bake
# the real path into compile_commands.json so clangd never needs to
# guess it.
execute_process(
  COMMAND ${CMAKE_C_COMPILER} -print-file-name=include
  OUTPUT_VARIABLE CC_BUILTIN_INCLUDE_DIR
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(EXISTS "${CC_BUILTIN_INCLUDE_DIR}")
  add_compile_options(-isystem "${CC_BUILTIN_INCLUDE_DIR}")
endif()

find_package(Threads REQUIRED)

add_library(pftrace STATIC
  src/pb_writer.c
  src/pftrace.c
)
target_include_directories(pftrace PUBLIC include)
target_link_libraries(pftrace PUBLIC Threads::Threads)
set_target_properties(pftrace PROPERTIES
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

add_executable(example_basic examples/example_basic.c)
target_link_libraries(example_basic PRIVATE pftrace)
