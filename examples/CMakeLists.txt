add_executable(example_basic example_basic.c)
target_link_libraries(example_basic PRIVATE pftrace)
# usleep is a POSIX/BSD addition glibc hides under strict ISO C.
target_compile_definitions(example_basic PRIVATE _DEFAULT_SOURCE)

# Writes the demo_*.pftrace files the trace validators read.
add_test(NAME example_basic COMMAND example_basic
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(example_basic PROPERTIES FIXTURES_SETUP demo_traces)

add_executable(example_threads example_threads.c)
target_link_libraries(example_threads PRIVATE pftrace)

# Real worker threads; writes demo_threads.pftrace next to the other demos.
add_test(NAME example_threads COMMAND example_threads
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(example_threads PROPERTIES FIXTURES_SETUP demo_traces)
