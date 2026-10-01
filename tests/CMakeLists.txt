# Ring mechanics in isolation: compiles pf_ring.c directly rather than
# linking pftrace, so the ring can be tested without protobuf, files or any
# of the library's state. That is also why it needs src/ on the include
# path -- pf_ring.h is private to the implementation.
add_executable(test_ring
  test_ring.c
  ${PROJECT_SOURCE_DIR}/src/pf_ring.c
)
target_include_directories(test_ring PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_ring PRIVATE Threads::Threads)
target_compile_definitions(test_ring PRIVATE _GNU_SOURCE)

# Drives enough events through a small ring to force many wraps, which is
# where snapshot windowing and continuous gaplessness actually get tested.
# memmem, used to confirm which events survived in a window, needs
# _GNU_SOURCE.
add_executable(test_stress test_stress.c)
target_link_libraries(test_stress PRIVATE pftrace Threads::Threads)
target_compile_definitions(test_stress PRIVATE _GNU_SOURCE)

# Interposes malloc/calloc/realloc to prove the hot path allocates nothing
# once the trace is open -- requirement 1. Needs dl for RTLD_NEXT.
add_executable(test_noalloc test_noalloc.c)
target_link_libraries(test_noalloc
  PRIVATE pftrace Threads::Threads ${CMAKE_DL_LIBS}
)

# Not a test: reports producer vs drain cost per event, to show where the
# hot path actually stands and at what rate continuous mode starts dropping.
add_executable(bench bench.c)
target_link_libraries(bench PRIVATE pftrace Threads::Threads)

add_test(NAME ring COMMAND test_ring)
add_test(NAME stress COMMAND test_stress)
add_test(NAME noalloc COMMAND test_noalloc)
