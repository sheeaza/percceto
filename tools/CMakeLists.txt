# Validates a trace against the protobuf wire format without needing the
# perfetto Python package, which is not installable everywhere. Doubles as
# the CI gate; verify_trace.py is the stronger check where that package is
# available, since it parses against perfetto's own generated schema.
#
# Deliberately does not link pftrace: decoding with the encoder's own
# constants would let a wrong field number agree with itself.
add_executable(dump_trace dump_trace.c)

# Each trace is written by a test that sets the fixture these need. Run from
# the build root, where the traces are written.
foreach(trace stress_snapshot stress_continuous stress_paced stress_names)
  add_test(NAME dump_${trace} COMMAND dump_trace ${trace}.pftrace
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(dump_${trace} PROPERTIES FIXTURES_REQUIRED stress_traces)
endforeach()
foreach(trace demo_snapshot demo_continuous)
  add_test(NAME dump_${trace} COMMAND dump_trace ${trace}.pftrace
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(dump_${trace} PROPERTIES FIXTURES_REQUIRED demo_traces)
endforeach()

# The same traces through verify_trace.py, which checks them against perfetto's
# generated schema. That needs the perfetto Python package, so the tests are
# added only when it imports.
find_program(PYTHON3 python3)
set(have_perfetto 1)
if(PYTHON3)
  execute_process(COMMAND ${PYTHON3} -c "import perfetto"
    RESULT_VARIABLE have_perfetto OUTPUT_QUIET ERROR_QUIET)
endif()

if(PYTHON3 AND have_perfetto EQUAL 0)
  add_test(NAME verify_trace_continuous COMMAND ${PYTHON3}
    ${PROJECT_SOURCE_DIR}/tools/verify_trace.py --expect-complete
    demo_continuous.pftrace WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  add_test(NAME verify_trace_snapshot COMMAND ${PYTHON3}
    ${PROJECT_SOURCE_DIR}/tools/verify_trace.py demo_snapshot.pftrace
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(verify_trace_continuous verify_trace_snapshot
    PROPERTIES FIXTURES_REQUIRED demo_traces)

  # stress_continuous runs saturated and drops events, so it gets --allow-drops:
  # its begins and ends do not balance.
  foreach(trace stress_snapshot stress_paced stress_names)
    add_test(NAME verify_${trace} COMMAND ${PYTHON3}
      ${PROJECT_SOURCE_DIR}/tools/verify_trace.py ${trace}.pftrace
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
    set_tests_properties(verify_${trace} PROPERTIES
      FIXTURES_REQUIRED stress_traces)
  endforeach()
  add_test(NAME verify_stress_continuous COMMAND ${PYTHON3}
    ${PROJECT_SOURCE_DIR}/tools/verify_trace.py --allow-drops
    stress_continuous.pftrace WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(verify_stress_continuous PROPERTIES
    FIXTURES_REQUIRED stress_traces)
else()
  message(STATUS "perfetto Python package not found: verify_trace tests not added")
endif()

# The raw-to-protobuf converter. Its unit tests need only the standard
# library, so they run wherever python3 does. The end-to-end test converts
# the raw file the raw_roundtrip test writes, and checks the result with
# dump_trace, which validates the wire format without any library.
if(PYTHON3 AND have_perfetto EQUAL 0)
  add_test(NAME pfraw2pb_unit COMMAND ${PYTHON3} -m unittest discover
    -s ${PROJECT_SOURCE_DIR}/tools -p test_pfraw2pb.py
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

  add_test(NAME pfraw2pb_convert COMMAND ${PYTHON3}
    ${PROJECT_SOURCE_DIR}/tools/pfraw2pb.py raw_roundtrip_continuous.pfraw
    -o raw_roundtrip_continuous.converted.pftrace
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(pfraw2pb_convert PROPERTIES FIXTURES_REQUIRED raw_traces)

  add_test(NAME dump_raw_roundtrip COMMAND dump_trace
    raw_roundtrip_continuous.converted.pftrace
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  set_tests_properties(dump_raw_roundtrip PROPERTIES DEPENDS pfraw2pb_convert)
endif()
