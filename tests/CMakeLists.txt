# Each test below is built against libpftrace and runs in PFTRACE_RUN_DIR, where
# the dump_trace and verify_trace tests look for the traces it writes.
#
# Drives enough events through a small buffer to force many chunk rotations
# and drops, which is where snapshot windowing and continuous gaplessness
# actually get tested. memmem, used to confirm which events survived in a
# window, needs _GNU_SOURCE.

add_executable(test_stress test_stress.c)
target_link_libraries(test_stress PRIVATE pftrace Threads::Threads)
target_compile_definitions(test_stress PRIVATE _GNU_SOURCE)
# Writes the stress_*.pftrace files the dump_trace tests read.
add_test(NAME stress COMMAND test_stress WORKING_DIRECTORY ${PFTRACE_RUN_DIR})
set_tests_properties(stress PROPERTIES FIXTURES_SETUP stress_traces)

# Interposes malloc/calloc/realloc to prove the hot path allocates nothing
# once the trace is open -- requirement 1. Needs dl for RTLD_NEXT.
add_executable(test_noalloc test_noalloc.c)
target_link_libraries(test_noalloc PRIVATE pftrace Threads::Threads ${CMAKE_DL_LIBS})
add_test(NAME noalloc COMMAND test_noalloc WORKING_DIRECTORY ${PFTRACE_RUN_DIR})

# Event arguments rendered into names by the protobuf drain.
add_executable(test_args_pb test_args_pb.c)
target_link_libraries(test_args_pb PRIVATE pftrace Threads::Threads)
add_test(NAME args_pb COMMAND test_args_pb WORKING_DIRECTORY ${PFTRACE_RUN_DIR})

add_executable(test_packet_clock test_packet_clock.c)
target_link_libraries(test_packet_clock PRIVATE pftrace Threads::Threads)
add_test(NAME packet_clock COMMAND test_packet_clock WORKING_DIRECTORY ${PFTRACE_RUN_DIR})

# Chunk pool sizing, writer limits, and the drop accounting.
add_executable(test_chunk test_chunk.c)
target_include_directories(test_chunk PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(test_chunk PRIVATE pftrace Threads::Threads)
add_test(NAME chunk COMMAND test_chunk WORKING_DIRECTORY ${PFTRACE_RUN_DIR})

# Counter tracks are identified by name: same name, same track. Covers the
# lookup under track_lock, including two threads creating one name at once.
add_executable(test_counter_name test_counter_name.c)
target_link_libraries(test_counter_name PRIVATE pftrace Threads::Threads)
target_include_directories(test_counter_name PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME counter_name COMMAND test_counter_name)

# The simple API's thread tracks must have the process track as parent.
# Reads the internal track table, since the public API does not expose the
# process uuid.
add_executable(test_process_track test_process_track.c)
target_link_libraries(test_process_track PRIVATE pftrace Threads::Threads)
target_include_directories(test_process_track PRIVATE ${PROJECT_SOURCE_DIR}/src)
add_test(NAME process_track COMMAND test_process_track)

# The _g counter calls act on the default trace and the calling thread's
# track, and match their explicit counterparts.
add_executable(test_counter_g test_counter_g.c)
target_link_libraries(test_counter_g PRIVATE pftrace Threads::Threads)
add_test(NAME counter_g COMMAND test_counter_g)

# Round trip for the raw format: writes it, reads it back with a reader
# written from docs/raw_format.md, and compares every event. Its reader
# shares no code with the writer, so this checks the spec, not just that the
# two agree. Writes the raw_roundtrip_*.pfraw files the converter tests read.
add_executable(test_raw_roundtrip test_raw_roundtrip.c)
target_link_libraries(test_raw_roundtrip PRIVATE pftrace Threads::Threads)
target_compile_definitions(test_raw_roundtrip PRIVATE _GNU_SOURCE)
add_test(NAME raw_roundtrip COMMAND test_raw_roundtrip WORKING_DIRECTORY ${PFTRACE_RUN_DIR})
set_tests_properties(raw_roundtrip PROPERTIES FIXTURES_SETUP raw_traces)

# Not a test: reports producer vs drain cost per event, to show where the
# hot path actually stands and at what rate continuous mode starts dropping.
add_executable(bench bench.c)
target_link_libraries(bench PRIVATE pftrace Threads::Threads)

# Cost of the record-size change and of event arguments; see the file header.
add_executable(bench_args bench_args.c)
target_link_libraries(bench_args PRIVATE pftrace Threads::Threads)

# Chunk-buffer cost: rotation per chunk_slots, and scaling across threads.
add_executable(bench_chunk bench_chunk.c)
target_link_libraries(bench_chunk PRIVATE pftrace Threads::Threads)

# Producer and consumer cost per event in continuous mode, with the drain live.
add_executable(bench_side bench_side.c)
target_link_libraries(bench_side PRIVATE pftrace Threads::Threads)

# Prototype for TODO item 3: protobuf encoding cost on the producer.
add_executable(bench_encode bench_encode.c)
target_link_libraries(bench_encode PRIVATE pftrace)
target_include_directories(bench_encode PRIVATE ${PROJECT_SOURCE_DIR}/src)

# The stress test and the selected tests again, against the sanitized
# libraries, so ASan and TSan see their accesses. Each sanitized build runs in
# its own directory under PFTRACE_RUN_DIR, so its trace files do not collide
# with the plain build's. test_noalloc is left out: it interposes malloc, which
# ASan replaces with its own.
if(PFTRACE_BUILD_SANITIZED)
  set(sanitized_tests
    test_chunk
    test_counter_name
    test_process_track
    test_counter_g
    test_args_pb
    test_raw_roundtrip
  )
  foreach(sanitizer asan tsan)
    set(run_dir ${PFTRACE_RUN_DIR}/${sanitizer})
    file(MAKE_DIRECTORY ${run_dir})

    add_executable(test_stress_${sanitizer} test_stress.c)
    target_link_libraries(test_stress_${sanitizer}
      PRIVATE pftrace_${sanitizer} Threads::Threads
    )
    target_compile_definitions(test_stress_${sanitizer} PRIVATE
      _GNU_SOURCE PF_TRACE_PREFIX="stress_${sanitizer}_"
    )
    add_test(NAME stress_${sanitizer}
      COMMAND test_stress_${sanitizer}
    )

    foreach(name ${sanitized_tests})
      set(target ${name}_${sanitizer})
      add_executable(${target} ${name}.c)
      target_link_libraries(${target}
        PRIVATE pftrace_${sanitizer} Threads::Threads
      )
      target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/src)
      if(name STREQUAL "test_raw_roundtrip")
        target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
      endif()
      add_test(NAME ${target} COMMAND ${target} WORKING_DIRECTORY ${run_dir})
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
