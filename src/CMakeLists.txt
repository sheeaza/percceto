# The library. One build, no backend choice: see docs/chunk_buffers_design.md.
#
# Sources are listed relative to this directory, so the paths stay correct
# if the directory is ever vendored into another project.

set(PFTRACE_SOURCES
  pb_writer.c
  pf_chunk.c
  pf_emit.c
  pf_output.c
  pf_pb_drain.c
  pf_pb_encode.c
  pf_raw.c
  pf_track.c
  pftrace.c
)

add_library(pftrace STATIC ${PFTRACE_SOURCES})

target_include_directories(pftrace
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(pftrace PUBLIC Threads::Threads)

# clock_gettime/CLOCK_BOOTTIME (pftrace.c) is not ISO C; _DEFAULT_SOURCE
# exposes it. PRIVATE: the public header is self-contained and needs no
# feature macro from consumers.
target_compile_definitions(pftrace PRIVATE _DEFAULT_SOURCE)

# pf_tls is accessed only from these sources. initial-exec gives each access
# a fixed TLS offset, so it does not go through __tls_get_addr when the
# library is linked into a shared object.
target_compile_options(pftrace PRIVATE -ftls-model=initial-exec)

set_target_properties(pftrace PROPERTIES
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

# Shared copy of the library, libpftrace.so. Same sources as the static
# library; SHARED implies -fPIC. Only PF_API symbols are exported, because the
# preset above hides the rest. The initial-exec TLS model is left off: a
# dlopen'ed object with initial-exec TLS can fail when static TLS runs out
# (docs/TODO_done.md). Output name is pftrace so the file is libpftrace.so.
add_library(pftrace_shared SHARED ${PFTRACE_SOURCES})
set_target_properties(pftrace_shared PROPERTIES
  OUTPUT_NAME pftrace
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)
target_include_directories(pftrace_shared
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(pftrace_shared PUBLIC Threads::Threads)
target_compile_definitions(pftrace_shared PRIVATE _DEFAULT_SOURCE)

# Sanitized copy of the library, for the stress tests in tests/ to link. The
# whole library is instrumented, so the sanitizer sees every access the
# stress test drives through it.
#
# The flags are PUBLIC, so a test linking the library is instrumented and
# links the sanitizer runtime with no further setup.
#
#   name      target suffix, e.g. _asan
#   sanitizer the -fsanitize= value
function(pftrace_add_sanitized name sanitizer)
  set(target pftrace${name})
  add_library(${target} STATIC ${PFTRACE_SOURCES})
  target_include_directories(${target}
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  )
  target_link_libraries(${target} PUBLIC Threads::Threads)
  target_compile_definitions(${target} PRIVATE _DEFAULT_SOURCE)

  # -O1 keeps reports readable. -fno-sanitize-recover makes UBSan abort on
  # its first report instead of printing it and carrying on.
  target_compile_options(${target} PUBLIC
    -O1 -g -fsanitize=${sanitizer} -fno-sanitize-recover=all)
  target_link_options(${target} PUBLIC -fsanitize=${sanitizer})
endfunction()

if(PFTRACE_BUILD_SANITIZED)
  pftrace_add_sanitized(_asan address,undefined)
  pftrace_add_sanitized(_tsan thread)
endif()
