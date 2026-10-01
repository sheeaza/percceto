add_executable(example_basic example_basic.c)
target_link_libraries(example_basic PRIVATE pftrace)
# usleep is a POSIX/BSD addition glibc hides under strict ISO C.
target_compile_definitions(example_basic PRIVATE _DEFAULT_SOURCE)
