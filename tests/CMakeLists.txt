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

# The name contract is enforced by the compiler, so the test for it has to
# be a compilation. Each mode is an object-only build of the same file:
# name_literal_ok must succeed, and the other three must fail, which is
# what WILL_FAIL asserts. Without the negative cases a macro that stopped
# rejecting anything would look exactly like a passing test suite.
#
# EXCLUDE_FROM_ALL so a deliberately-failing compile is not part of a
# normal build; each test drives its own via --target.
foreach(mode OK VARIABLE STACK_BUFFER EMPTY)
  string(TOLOWER "${mode}" lower)
  add_library(name_literal_${lower} OBJECT EXCLUDE_FROM_ALL
    test_name_literal.c
  )
  target_link_libraries(name_literal_${lower} PRIVATE pftrace)
  target_compile_definitions(name_literal_${lower} PRIVATE MODE_${mode})

  add_test(NAME name_literal_${lower}
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}
            --target name_literal_${lower} --config $<CONFIG>
  )
endforeach()

# Only the literal form may compile.
set_tests_properties(name_literal_variable name_literal_stack_buffer
                     name_literal_empty
  PROPERTIES WILL_FAIL TRUE
)
