cmake_minimum_required(VERSION 3.10)
project(pftrace C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

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
