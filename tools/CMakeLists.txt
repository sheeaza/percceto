# Validates a trace against the protobuf wire format without needing the
# perfetto Python package, which is not installable everywhere. Doubles as
# the CI gate; import_trace.py is the stronger check where that package is
# available, since it parses against perfetto's own generated schema and
# imports the trace with trace_processor.
#
# Deliberately does not link pftrace: decoding with the encoder's own
# constants would let a wrong field number agree with itself.
add_executable(dump_trace dump_trace.c)

foreach(trace stress_snapshot stress_continuous stress_paced stress_names)
  add_test(NAME dump_${trace} COMMAND dump_trace ${trace}.pftrace
    WORKING_DIRECTORY ${PFTRACE_RUN_DIR})
  set_tests_properties(dump_${trace} PROPERTIES
    FIXTURES_REQUIRED stress_traces)
endforeach()
foreach(trace demo_snapshot demo_continuous)
  add_test(NAME dump_${trace} COMMAND dump_trace ${trace}.pftrace
    WORKING_DIRECTORY ${PFTRACE_RUN_DIR})
  set_tests_properties(dump_${trace} PROPERTIES
    FIXTURES_REQUIRED demo_traces)
endforeach()

# The same traces through import_trace.py, which checks them against perfetto's
# generated schema and trace_processor's import. That needs the perfetto Python
# package, so the tests are added only when it imports.
find_program(PYTHON3 python3)
set(have_perfetto 1)
if(PYTHON3)
  execute_process(COMMAND ${PYTHON3} -c "import perfetto"
    RESULT_VARIABLE have_perfetto OUTPUT_QUIET ERROR_QUIET)
endif()

if(PYTHON3 AND have_perfetto EQUAL 0)
  # Each test runs the file checks and then the trace_processor import checks.
  # The import checks need trace_processor_shell on PATH; without it a test
  # whose file checks pass exits 77 and is reported as skipped.
  find_program(TRACE_PROCESSOR_SHELL trace_processor_shell)
  if(NOT TRACE_PROCESSOR_SHELL)
    message(WARNING "trace_processor_shell not found: install Perfetto's "
      "trace processor to run the import checks; the import_* tests run "
      "only the file checks without it, and report as skipped")
  endif()

  add_test(NAME import_demo_continuous COMMAND ${PYTHON3}
    ${PROJECT_SOURCE_DIR}/tools/import_trace.py --expect-complete
    demo_continuous.pftrace WORKING_DIRECTORY ${PFTRACE_RUN_DIR})
  add_test(NAME import_demo_snapshot COMMAND ${PYTHON3}
    ${PROJECT_SOURCE_DIR}/tools/import_trace.py demo_snapshot.pftrace
    WORKING_DIRECTORY ${PFTRACE_RUN_DIR})
  add_test(NAME import_demo_threads COMMAND ${PYTHON3}
    ${PROJECT_SOURCE_DIR}/tools/import_trace.py --min-threads 2
    demo_threads.pftrace WORKING_DIRECTORY ${PFTRACE_RUN_DIR})
  set_tests_properties(import_demo_continuous import_demo_snapshot
    import_demo_threads PROPERTIES
    FIXTURES_REQUIRED demo_traces SKIP_RETURN_CODE 77)

  foreach(trace stress_snapshot stress_paced stress_names)
    add_test(NAME import_${trace} COMMAND ${PYTHON3}
      ${PROJECT_SOURCE_DIR}/tools/import_trace.py ${trace}.pftrace
      WORKING_DIRECTORY ${PFTRACE_RUN_DIR})
    set_tests_properties(import_${trace} PROPERTIES
      FIXTURES_REQUIRED stress_traces SKIP_RETURN_CODE 77)
  endforeach()
  # stress_continuous runs saturated and drops events, so it gets
  # --allow-drops: its begins and ends do not balance.
  add_test(NAME import_stress_continuous COMMAND ${PYTHON3}
    ${PROJECT_SOURCE_DIR}/tools/import_trace.py --allow-drops --min-threads 4
    stress_continuous.pftrace WORKING_DIRECTORY ${PFTRACE_RUN_DIR})
  set_tests_properties(import_stress_continuous PROPERTIES
    FIXTURES_REQUIRED stress_traces SKIP_RETURN_CODE 77)
  # Event timestamps are deltas on an incremental clock, so a wrong delta or a
  # clock snapshot Perfetto rejects shows only as wrong times after import.
  # test_timestamps records the time each event was stamped with.
  add_test(NAME import_timestamps COMMAND ${PYTHON3}
    ${PROJECT_SOURCE_DIR}/tools/import_trace.py --min-threads 4
    --expect-ts timestamps.d/timestamps.expected timestamps.d/timestamps.pftrace
    WORKING_DIRECTORY ${PFTRACE_RUN_DIR})
  set_tests_properties(import_timestamps PROPERTIES
    FIXTURES_REQUIRED timestamp_traces SKIP_RETURN_CODE 77)
else()
  message(STATUS "perfetto Python package not found: import_trace tests not added")
endif()

# The raw-to-protobuf converter. Its unit tests need only the standard
# library, so they run wherever python3 does. The end-to-end tests convert
# every raw file the tests write, and check each result with dump_trace,
# which validates the wire format without any library, and with
# import_trace.py, which checks it against perfetto's schema and importer.
if(PYTHON3 AND have_perfetto EQUAL 0)
  add_test(NAME pfraw2pb_unit COMMAND ${PYTHON3} -m unittest discover
    -s ${PROJECT_SOURCE_DIR}/tools -p test_pfraw2pb.py
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

  # Each raw file and the fixture that writes it. Every one holds the whole
  # sequence its test emitted, so the converted traces must balance exactly,
  # the snapshot too. raw_roundtrip_bad_args is left out: it is written with a
  # record the reader must reject, and pfraw2pb.py does.
  set(raw_traces_files
    raw_roundtrip_continuous raw_roundtrip_snapshot raw_roundtrip_header
    raw_roundtrip_torn raw_roundtrip_dyn raw_roundtrip_args
    raw_roundtrip_simple_threads raw_roundtrip_simple_rename)
  set(args_traces_files args_raw)
  foreach(fixture raw_traces args_traces)
    foreach(trace IN LISTS ${fixture}_files)
      add_test(NAME pfraw2pb_${trace} COMMAND ${PYTHON3}
        ${PROJECT_SOURCE_DIR}/tools/pfraw2pb.py ${trace}.pfraw
        -o ${trace}.converted.pftrace
        WORKING_DIRECTORY ${PFTRACE_RUN_DIR})
      set_tests_properties(pfraw2pb_${trace} PROPERTIES
        FIXTURES_REQUIRED ${fixture} FIXTURES_SETUP converted_${trace})

      add_test(NAME dump_${trace} COMMAND dump_trace ${trace}.converted.pftrace
        WORKING_DIRECTORY ${PFTRACE_RUN_DIR})
      set_tests_properties(dump_${trace} PROPERTIES
        FIXTURES_REQUIRED converted_${trace})

      add_test(NAME import_${trace} COMMAND ${PYTHON3}
        ${PROJECT_SOURCE_DIR}/tools/import_trace.py --expect-complete
        ${trace}.converted.pftrace
        WORKING_DIRECTORY ${PFTRACE_RUN_DIR})
      set_tests_properties(import_${trace} PROPERTIES
        FIXTURES_REQUIRED converted_${trace} SKIP_RETURN_CODE 77)
    endforeach()
  endforeach()

  # test_rotate's raw run, as many files as the size limit cut it into. Each
  # file has its own header, so each converts alone.
  add_test(NAME pfraw2pb_rotate_raw COMMAND ${CMAKE_COMMAND}
    -DDIR=${PFTRACE_RUN_DIR}/rotate_raw -DPYTHON3=${PYTHON3}
    -DTOOLS=${PROJECT_SOURCE_DIR}/tools -DDUMP_TRACE=$<TARGET_FILE:dump_trace>
    -P ${PROJECT_SOURCE_DIR}/tools/convert_raw_dir.cmake)
  set_tests_properties(pfraw2pb_rotate_raw PROPERTIES
    FIXTURES_REQUIRED rotate_traces SKIP_REGULAR_EXPRESSION "import checks skipped")
endif()
