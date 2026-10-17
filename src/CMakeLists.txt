# The library, once per ring backend.
#
# All three link the same pftrace.c, pb_writer.c and public header, and
# differ only in which ring implements src/pf_ring.h. That is what makes
# BENCHMARK_ring_backends.md a comparison of rings rather than of
# libraries: everything above the ring is the same code.
#
# `pftrace` is the default target and the one every test and tool links.
# The other two exist to be benchmarked, and are built only when
# PFTRACE_BUILD_BENCH is on -- they are not alternative products.
#
# Sources are listed relative to this directory, so the paths stay correct
# if the directory is ever vendored into another project.

# Sources common to every variant of the library. perccetto.pb.c is generated
# from proto/perccetto.proto; nanopb/ holds the vendored encoder it uses. See
# src/VENDOR.md.
set(PFTRACE_COMMON_SOURCES
  pb_writer.c pftrace.c perccetto.pb.c nanopb/pb_common.c nanopb/pb_encode.c)

# One function body per backend, parameterized so a fourth is a one-liner.
#
#   name      target suffix, empty for the default
#   ring      the .c implementing pf_ring.h
#   define    the selector pf_ring.h switches on, empty for the default
#   extra     ringbuf library the backend wraps, empty for none
function(pftrace_add_library name ring define extra)
  set(target pftrace${name})
  add_library(${target} STATIC ${PFTRACE_COMMON_SOURCES} ${ring})
  if(extra)
    target_link_libraries(${target} PUBLIC ${extra})
  endif()

  # include/ is PUBLIC (consumers need the API header); this directory is
  # PRIVATE, holding pf_ring.h, the backend it selects and pb_writer.h,
  # which are implementation details. nanopb/ is PRIVATE too: the generated
  # perccetto.pb.h includes <pb.h> from it.
  target_include_directories(${target}
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/nanopb
  )
  target_link_libraries(${target} PUBLIC Threads::Threads)

  # clock_gettime/CLOCK_BOOTTIME (pftrace.c) is not ISO C; _DEFAULT_SOURCE
  # exposes it. Without it, a strict
  # -std=c11 build fails on implicit declarations. PRIVATE: the public header
  # is self-contained and needs no feature macro from consumers.
  #
  # The vendored ringbuf sources declare their own _POSIX_C_SOURCE for
  # posix_memalign and need nothing from here.
  target_compile_definitions(${target} PRIVATE _DEFAULT_SOURCE)
  if(define)
    # PUBLIC, not PRIVATE: pf_ring.h picks the backend's struct definition
    # from this, so anything compiling against these sources -- the bench
    # binaries -- must see the same selector or it would embed the wrong
    # struct. The public pftrace.h is unaffected either way.
    target_compile_definitions(${target} PUBLIC ${define})
  endif()

  set_target_properties(${target} PROPERTIES
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
  )
endfunction()

pftrace_add_library("" pf_ring_mpsc.c "" ringbuf_mpsc)

# The default is ringbuf_mpsc, so the mpsc bench target is an alias for it.
# It exists so the bench loops in tests/ can address every backend by name.
add_library(pftrace_mpsc ALIAS pftrace)

if(PFTRACE_BUILD_BENCH)
  pftrace_add_library(_zcopy pf_ring_zcopy.c PF_RING_BACKEND_ZCOPY
                      ringbuf_zcopy)
endif()

# Sanitized copies of the library, one per backend and sanitizer, for the
# stress tests in tests/ to link. Unlike the bench variants these change
# what gets compiled, so the whole library is instrumented -- including the
# ringbuf source each ring wraps, compiled in directly rather than linked,
# so its own accesses are checked too.
#
# The flags are PUBLIC, so a test linking the library is instrumented and
# links the sanitizer runtime with no further setup.
#
#   name      target suffix, e.g. _mpsc_asan
#   ring      the .c implementing pf_ring.h
#   ringbuf   the ringbuf .c that ring wraps
#   define    the selector pf_ring.h switches on, empty for the default
#   sanitizer the -fsanitize= value
function(pftrace_add_sanitized name ring ringbuf define sanitizer)
  set(target pftrace${name})
  add_library(${target} STATIC ${PFTRACE_COMMON_SOURCES} ${ring} ${ringbuf})
  target_include_directories(${target}
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/nanopb
      ${PROJECT_SOURCE_DIR}/ringbuf
  )
  target_link_libraries(${target} PUBLIC Threads::Threads)
  target_compile_definitions(${target} PRIVATE _DEFAULT_SOURCE)
  if(define)
    target_compile_definitions(${target} PUBLIC ${define})
  endif()

  # -O1 keeps reports readable. -fno-sanitize-recover makes UBSan abort on
  # its first report instead of printing it and carrying on.
  target_compile_options(${target} PUBLIC
    -O1 -g -fsanitize=${sanitizer} -fno-sanitize-recover=all)
  target_link_options(${target} PUBLIC -fsanitize=${sanitizer})
endfunction()

if(PFTRACE_BUILD_SANITIZED)
  pftrace_add_sanitized(_mpsc_asan pf_ring_mpsc.c ../ringbuf/ringbuf_mpsc.c
                        "" address,undefined)
  pftrace_add_sanitized(_zcopy_asan pf_ring_zcopy.c ../ringbuf/ringbuf_zcopy.c
                        PF_RING_BACKEND_ZCOPY address,undefined)
  pftrace_add_sanitized(_mpsc_tsan pf_ring_mpsc.c ../ringbuf/ringbuf_mpsc.c
                        "" thread)
  pftrace_add_sanitized(_zcopy_tsan pf_ring_zcopy.c ../ringbuf/ringbuf_zcopy.c
                        PF_RING_BACKEND_ZCOPY thread)
endif()
