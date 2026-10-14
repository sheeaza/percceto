# Validates a trace against the protobuf wire format without needing the
# perfetto Python package, which is not installable everywhere. Doubles as
# the CI gate; verify_trace.py and check_schema.py are the stronger checks
# where a real generated schema is available.
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

# The strongest schema check there is: parse our bytes with code generated
# from the real field numbers, so an encoder bug cannot hide behind a matching
# bug in dump_trace. That needs a protobuf runtime, so each check is added only
# when one is found: the perfetto Python package, or nix-shell to fetch protoc.
find_program(PYTHON3 python3)
find_program(NIX_SHELL nix-shell)
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
elseif(NIX_SHELL)
  # check_schema.sh takes the build dir relative to the source root.
  file(RELATIVE_PATH build_rel ${PROJECT_SOURCE_DIR} ${CMAKE_BINARY_DIR})
  add_test(NAME schema_check COMMAND ${NIX_SHELL} -p protobuf
    "python3.withPackages(p: [p.protobuf])"
    --run "bash ${PROJECT_SOURCE_DIR}/tools/check_schema.sh ${PROJECT_SOURCE_DIR} ${build_rel}")
  set_tests_properties(schema_check PROPERTIES
    FIXTURES_REQUIRED "demo_traces;stress_traces")
else()
  message(STATUS "No protobuf runtime found: schema check not added")
endif()
