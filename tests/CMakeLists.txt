# Drives enough events through a small ring to force many wraps, which is
# where snapshot windowing and continuous gaplessness actually get tested.
# memmem, used to confirm which events survived in a window, needs
# _GNU_SOURCE.
#
# Built once per backend when the benchmarks are on: these two use the
# public API only, so they are the correctness gate for an alternative ring
# -- each writes its traces to its own paths, so the variants can run
# concurrently under ctest. The traces the dump_trace tests validate come
# from the default build's test_stress.
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


# Writes the stress_*.pftrace files the dump_trace tests read, so it runs in
# the build root, where they look for them.
add_test(NAME stress COMMAND test_stress WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(stress PROPERTIES FIXTURES_SETUP stress_traces)
add_test(NAME noalloc COMMAND test_noalloc)

# Counter tracks are identified by name: same name, same track. Covers the
# lookup under track_lock, including two threads creating one name at once.
add_executable(test_counter_name test_counter_name.c)
target_link_libraries(test_counter_name PRIVATE pftrace Threads::Threads)
add_test(NAME counter_name COMMAND test_counter_name)

# The simple API's thread tracks must have the process track as parent.
# Reads the internal track table, since the public API does not expose the
# process uuid.
add_executable(test_process_track test_process_track.c)
target_link_libraries(test_process_track PRIVATE pftrace)
add_test(NAME process_track COMMAND test_process_track)

# The _sp counter calls act on the default trace and the calling thread's
# track, and match their explicit counterparts.
add_executable(test_counter_sp test_counter_sp.c)
target_link_libraries(test_counter_sp PRIVATE pftrace)
add_test(NAME counter_sp COMMAND test_counter_sp)

if(PFTRACE_BUILD_BENCH)
  foreach(backend mpsc zcopy)
    add_executable(test_stress_${backend} test_stress.c)
    target_link_libraries(test_stress_${backend}
      PRIVATE pftrace_${backend} Threads::Threads
    )
    target_compile_definitions(test_stress_${backend} PRIVATE
      _GNU_SOURCE PF_TRACE_PREFIX="stress_${backend}_"
    )
    add_test(NAME stress_${backend} COMMAND test_stress_${backend})

    add_executable(test_noalloc_${backend} test_noalloc.c)
    target_link_libraries(test_noalloc_${backend}
      PRIVATE pftrace_${backend} Threads::Threads ${CMAKE_DL_LIBS}
    )
    target_compile_definitions(test_noalloc_${backend} PRIVATE
      PF_TRACE_PREFIX="noalloc_${backend}_"
    )
    add_test(NAME noalloc_${backend} COMMAND test_noalloc_${backend})
  endforeach()

  # The ring comparison: one source, linked once per backend,
  # since the backend is a compile-time choice and `struct pf_ring` is a
  # different type in each -- one process cannot hold two of them.
  #
  # Deliberately NOT add_test entries. These are measurements, not pass/fail
  # checks: they take minutes, and their output is numbers to read rather
  # than a verdict. Keeping them out of ctest keeps ctest fast and its
  # results meaningful.
  #
  # Always -O2 -g regardless of CMAKE_BUILD_TYPE. Benchmarking a -O0 or -Og
  # build measures nothing useful, and a stray Debug configure would
  # otherwise produce garbage numbers silently.
  #
  # The backend selector reaches these through the library's PUBLIC compile
  # definitions, so bench_ring.c sees the same macro pf_ring.h switched on
  # and labels its rows correctly.
  foreach(target_backend mpsc zcopy)
    add_executable(bench_ring_${target_backend} bench_ring.c)
    target_link_libraries(bench_ring_${target_backend}
      PRIVATE pftrace_${target_backend} Threads::Threads
    )
    target_compile_options(bench_ring_${target_backend} PRIVATE -O2 -g)
  endforeach()
endif()

# The stress test under each sanitizer, against each backend's sanitized
# library. The library's PUBLIC flags instrument this file too, so the
# sanitizer sees the test's own accesses, not only the library's.
if(PFTRACE_BUILD_SANITIZED)
  foreach(backend mpsc zcopy)
    foreach(sanitizer asan tsan)
      add_executable(test_stress_${backend}_${sanitizer} test_stress.c)
      target_link_libraries(test_stress_${backend}_${sanitizer}
        PRIVATE pftrace_${backend}_${sanitizer} Threads::Threads
      )
      target_compile_definitions(test_stress_${backend}_${sanitizer} PRIVATE
        _GNU_SOURCE PF_TRACE_PREFIX="stress_${backend}_${sanitizer}_"
      )
      add_test(NAME stress_${backend}_${sanitizer}
        COMMAND test_stress_${backend}_${sanitizer}
      )
    endforeach()
  endforeach()
endif()

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

# Round trip for the raw format: writes it, reads it back with a reader
# written from docs/raw_format.md, and compares every event. Its reader
# shares no code with the writer, so this checks the spec, not just that the
# two agree.
add_executable(test_raw_roundtrip test_raw_roundtrip.c)
target_link_libraries(test_raw_roundtrip PRIVATE pftrace Threads::Threads)
target_compile_definitions(test_raw_roundtrip PRIVATE _GNU_SOURCE)
# Writes the raw_roundtrip_*.pfraw files the converter tests read, so it runs
# in the build root, where they look for them.
add_test(NAME raw_roundtrip COMMAND test_raw_roundtrip
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(raw_roundtrip PROPERTIES FIXTURES_SETUP raw_traces)
