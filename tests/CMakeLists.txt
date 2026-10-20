# Each test below is built once per backend, against libpftrace_<backend>, and
# runs in build/<backend>/, where the dump_trace and verify_trace tests look
# for the traces it writes.
#
# Drives enough events through a small ring to force many wraps, which is
# where snapshot windowing and continuous gaplessness actually get tested.
# memmem, used to confirm which events survived in a window, needs
# _GNU_SOURCE.
foreach(backend ${PFTRACE_BACKENDS})
  set(run_dir ${CMAKE_BINARY_DIR}/${backend})

  add_executable(test_stress_${backend} test_stress.c)
  target_link_libraries(test_stress_${backend}
    PRIVATE pftrace_${backend} Threads::Threads
  )
  target_compile_definitions(test_stress_${backend} PRIVATE _GNU_SOURCE)
  # Writes the stress_*.pftrace files the dump_trace tests read.
  add_test(NAME stress_${backend} COMMAND test_stress_${backend}
    WORKING_DIRECTORY ${run_dir})
  set_tests_properties(stress_${backend} PROPERTIES
    FIXTURES_SETUP stress_traces_${backend})

  # Interposes malloc/calloc/realloc to prove the hot path allocates nothing
  # once the trace is open -- requirement 1. Needs dl for RTLD_NEXT.
  add_executable(test_noalloc_${backend} test_noalloc.c)
  target_link_libraries(test_noalloc_${backend}
    PRIVATE pftrace_${backend} Threads::Threads ${CMAKE_DL_LIBS}
  )
  add_test(NAME noalloc_${backend} COMMAND test_noalloc_${backend}
    WORKING_DIRECTORY ${run_dir})

  # Event arguments rendered into names by the protobuf drain. Runs in its
  # backend's own directory: its dump directory is reset on start, so two
  # backends sharing one directory would delete each other's files under -j.
  add_executable(test_args_pb_${backend} test_args_pb.c)
  target_link_libraries(test_args_pb_${backend}
    PRIVATE pftrace_${backend} Threads::Threads
  )
  add_test(NAME args_pb_${backend} COMMAND test_args_pb_${backend}
    WORKING_DIRECTORY ${run_dir})

  add_executable(test_packet_clock_${backend} test_packet_clock.c)
  target_link_libraries(test_packet_clock_${backend}
    PRIVATE pftrace_${backend} Threads::Threads
  )
  add_test(NAME packet_clock_${backend} COMMAND test_packet_clock_${backend}
    WORKING_DIRECTORY ${run_dir})

  # The ring holds at least min_ring_bytes, at the slot-count boundary.
  add_executable(test_min_ring_${backend} test_min_ring.c)
  target_link_libraries(test_min_ring_${backend}
    PRIVATE pftrace_${backend} Threads::Threads
  )
  add_test(NAME min_ring_${backend} COMMAND test_min_ring_${backend})

  # Counter tracks are identified by name: same name, same track. Covers the
  # lookup under track_lock, including two threads creating one name at once.
  add_executable(test_counter_name_${backend} test_counter_name.c)
  target_link_libraries(test_counter_name_${backend}
    PRIVATE pftrace_${backend} Threads::Threads
  )
  add_test(NAME counter_name_${backend} COMMAND test_counter_name_${backend})

  # The simple API's thread tracks must have the process track as parent.
  # Reads the internal track table, since the public API does not expose the
  # process uuid.
  add_executable(test_process_track_${backend} test_process_track.c)
  target_link_libraries(test_process_track_${backend}
    PRIVATE pftrace_${backend} Threads::Threads
  )
  add_test(NAME process_track_${backend} COMMAND test_process_track_${backend})

  # The _g counter calls act on the default trace and the calling thread's
  # track, and match their explicit counterparts.
  add_executable(test_counter_g_${backend} test_counter_g.c)
  target_link_libraries(test_counter_g_${backend}
    PRIVATE pftrace_${backend} Threads::Threads
  )
  add_test(NAME counter_g_${backend} COMMAND test_counter_g_${backend})

  # Round trip for the raw format: writes it, reads it back with a reader
  # written from docs/raw_format.md, and compares every event. Its reader
  # shares no code with the writer, so this checks the spec, not just that the
  # two agree. Writes the raw_roundtrip_*.pfraw files the converter tests read.
  add_executable(test_raw_roundtrip_${backend} test_raw_roundtrip.c)
  target_link_libraries(test_raw_roundtrip_${backend}
    PRIVATE pftrace_${backend} Threads::Threads
  )
  target_compile_definitions(test_raw_roundtrip_${backend} PRIVATE _GNU_SOURCE)
  add_test(NAME raw_roundtrip_${backend} COMMAND test_raw_roundtrip_${backend}
    WORKING_DIRECTORY ${run_dir})
  set_tests_properties(raw_roundtrip_${backend} PROPERTIES
    FIXTURES_SETUP raw_traces_${backend})

  # Not a test: reports producer vs drain cost per event, to show where the
  # hot path actually stands and at what rate continuous mode starts dropping.
  add_executable(bench_${backend} bench.c)
  target_link_libraries(bench_${backend}
    PRIVATE pftrace_${backend} Threads::Threads
  )

  # Cost of the record-size change and of event arguments; see the file header.
  add_executable(bench_args_${backend} bench_args.c)
  target_link_libraries(bench_args_${backend}
    PRIVATE pftrace_${backend} Threads::Threads
  )
endforeach()

# Single 64-byte claim versus 32-byte claims, on the vendored ringbuf_mpsc.
# Compiles the ring directly and does not link the library, so it is built once.
add_executable(bench_slot_size bench_slot_size.c ${PROJECT_SOURCE_DIR}/ringbuf/ringbuf_mpsc.c)
target_include_directories(bench_slot_size PRIVATE ${PROJECT_SOURCE_DIR}/ringbuf)
target_link_libraries(bench_slot_size PRIVATE Threads::Threads)

if(PFTRACE_BUILD_BENCH)
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
  foreach(backend ${PFTRACE_BACKENDS})
    add_executable(bench_ring_${backend} bench_ring.c)
    target_link_libraries(bench_ring_${backend}
      PRIVATE pftrace_${backend} Threads::Threads
    )
    target_compile_options(bench_ring_${backend} PRIVATE -O2 -g)
  endforeach()
endif()

# The stress test under each sanitizer, against each backend's sanitized
# library. The library's PUBLIC flags instrument this file too, so the
# sanitizer sees the test's own accesses, not only the library's.
if(PFTRACE_BUILD_SANITIZED)
  foreach(backend ${PFTRACE_BACKENDS})
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

# Selected tests, built against each backend's sanitized library as well, so
# ASan and TSan see their accesses. Each sanitized build runs in its own
# directory, build/<backend>_<sanitizer>/, so its trace files do not collide
# with the plain build's. test_noalloc is left out: it interposes malloc, which
# ASan replaces with its own.
if(PFTRACE_BUILD_SANITIZED)
  set(sanitized_tests
    test_min_ring
    test_counter_name
    test_process_track
    test_counter_g
    test_args_pb
    test_raw_roundtrip
  )
  foreach(backend ${PFTRACE_BACKENDS})
    foreach(sanitizer asan tsan)
      set(run_dir ${CMAKE_BINARY_DIR}/${backend}_${sanitizer})
      file(MAKE_DIRECTORY ${run_dir})
      foreach(name ${sanitized_tests})
        set(target ${name}_${backend}_${sanitizer})
        add_executable(${target} ${name}.c)
        target_link_libraries(${target}
          PRIVATE pftrace_${backend}_${sanitizer} Threads::Threads
        )
        # The sanitized library compiles the ring sources in rather than
        # linking them, so tests that include pftrace_internal.h need the
        # ringbuf headers themselves.
        target_include_directories(${target}
          PRIVATE ${PROJECT_SOURCE_DIR}/ringbuf
        )
        if(name STREQUAL "test_raw_roundtrip")
          target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
        endif()
        add_test(NAME ${target} COMMAND ${target} WORKING_DIRECTORY ${run_dir})
      endforeach()
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
# normal build; each test drives its own via --target. Only the public header
# is needed, and it is the same for both backends.
foreach(mode OK VARIABLE STACK_BUFFER EMPTY)
  string(TOLOWER "${mode}" lower)
  add_library(name_literal_${lower} OBJECT EXCLUDE_FROM_ALL
    test_name_literal.c
  )
  target_include_directories(name_literal_${lower} PRIVATE
    ${PROJECT_SOURCE_DIR}/include
  )
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

# The C++ wrapper must compile as C++11. Object-only, so nothing is linked or
# run and the library is not needed. It is part of the default build: a header
# that stops compiling under C++11 breaks the build.
add_library(pftrace_hpp_compile OBJECT test_pftrace_hpp.cpp)
set_target_properties(pftrace_hpp_compile PROPERTIES
  CXX_STANDARD 11
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)
target_include_directories(pftrace_hpp_compile PRIVATE
  ${PROJECT_SOURCE_DIR}/include
)
