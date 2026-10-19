foreach(backend ${PFTRACE_BACKENDS})
  set(run_dir ${CMAKE_BINARY_DIR}/${backend})

  add_executable(example_basic_${backend} example_basic.c)
  target_link_libraries(example_basic_${backend} PRIVATE pftrace_${backend})
  # usleep is a POSIX/BSD addition glibc hides under strict ISO C.
  target_compile_definitions(example_basic_${backend} PRIVATE _DEFAULT_SOURCE)

  # Writes the demo_*.pftrace files the trace validators read.
  add_test(NAME example_basic_${backend} COMMAND example_basic_${backend}
    WORKING_DIRECTORY ${run_dir})
  set_tests_properties(example_basic_${backend} PROPERTIES
    FIXTURES_SETUP demo_traces_${backend})

  add_executable(example_threads_${backend} example_threads.c)
  target_link_libraries(example_threads_${backend} PRIVATE pftrace_${backend})

  # Real worker threads; writes demo_threads.pftrace next to the other demos.
  add_test(NAME example_threads_${backend} COMMAND example_threads_${backend}
    WORKING_DIRECTORY ${run_dir})
  set_tests_properties(example_threads_${backend} PROPERTIES
    FIXTURES_SETUP demo_traces_${backend})
endforeach()
