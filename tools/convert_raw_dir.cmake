# Converts every file in DIR with pfraw2pb.py and checks each result with
# dump_trace, then checks them all with import_trace.py as one trace. For a
# raw trace written as several rotated files, whose count depends on the run.
# Each file has its own header, so each converts alone. A Trace is a repeated
# packet field, so the converted files joined in order parse as one Trace,
# and one import costs one trace_processor start rather than one per file.
#
#   cmake -DDIR=... -DPYTHON3=... -DTOOLS=... -DDUMP_TRACE=... -P convert_raw_dir.cmake
#
# Prints "import checks skipped" when import_trace.py skipped them, which the
# test matches with SKIP_REGULAR_EXPRESSION; a script cannot set its own exit
# code before CMake 3.29.

file(GLOB inputs "${DIR}/*")
list(FILTER inputs EXCLUDE REGEX "\\.converted$")
list(LENGTH inputs count)
if(count EQUAL 0)
  message(FATAL_ERROR "no raw files in ${DIR}")
endif()

# Rotation names the files trace.pftrace, then .1, .2 and on. Take them in
# that order, not by name, so the joined trace keeps time order.
set(ordered "")
math(EXPR last "${count} - 1")
foreach(i RANGE ${last})
  if(i EQUAL 0)
    set(suffix "")
  else()
    set(suffix ".${i}")
  endif()
  foreach(in IN LISTS inputs)
    if(in MATCHES "\\.pftrace${suffix}$")
      list(APPEND ordered ${in})
    endif()
  endforeach()
endforeach()
list(LENGTH ordered n)
if(NOT n EQUAL count)
  message(FATAL_ERROR "${DIR}: ${count} files, but ${n} follow the rotation names")
endif()

set(converted "")
foreach(in IN LISTS ordered)
  set(out "${in}.converted")
  execute_process(COMMAND ${PYTHON3} ${TOOLS}/pfraw2pb.py ${in} -o ${out}
    RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "pfraw2pb.py failed on ${in}")
  endif()
  execute_process(COMMAND ${DUMP_TRACE} ${out} RESULT_VARIABLE rc OUTPUT_QUIET)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "dump_trace failed on ${out}")
  endif()
  list(APPEND converted ${out})
endforeach()
message(STATUS "${count} raw files converted, each passes dump_trace")

set(all "${DIR}/all.converted")
execute_process(COMMAND ${CMAKE_COMMAND} -E cat ${converted}
  OUTPUT_FILE ${all} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "could not join the converted files into ${all}")
endif()

execute_process(COMMAND ${PYTHON3} ${TOOLS}/import_trace.py --expect-complete ${all}
  RESULT_VARIABLE rc OUTPUT_VARIABLE log ERROR_VARIABLE log)
message("${log}")
if(rc EQUAL 77)
  message(STATUS "import checks skipped")
elseif(NOT rc EQUAL 0)
  message(FATAL_ERROR "import_trace.py failed on ${all}")
endif()
