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

# One function body per backend, parameterized so a fourth is a one-liner.
#
#   name      target suffix, empty for the default
#   ring      the .c implementing pf_ring.h
#   define    the selector pf_ring.h switches on, empty for the default
#   extra     ringbuf library the backend wraps, empty for none
function(pftrace_add_library name ring define extra)
  set(target pftrace${name})
  add_library(${target} STATIC pb_writer.c pftrace.c ${ring})
  if(extra)
    target_link_libraries(${target} PUBLIC ${extra})
  endif()

  # include/ is PUBLIC (consumers need the API header); this directory is
  # PRIVATE, holding pf_ring.h, the backend it selects and pb_writer.h,
  # which are implementation details. tests/test_ring.c reaches in here
  # deliberately -- see its own CMakeLists.txt.
  target_include_directories(${target}
    PUBLIC ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
  )
  target_link_libraries(${target} PUBLIC Threads::Threads)

  # memfd_create (the mmap ring's double mapping) is glibc's, not ISO C's,
  # and unlike clock_gettime/CLOCK_BOOTTIME it is not exposed by
  # _DEFAULT_SOURCE -- only _GNU_SOURCE declares it. Without this the build
  # fails on an implicit declaration. PRIVATE: the public header is
  # self-contained and needs no feature macro from consumers.
  #
  # The vendored ringbuf sources declare their own _POSIX_C_SOURCE for
  # posix_memalign and need nothing from here.
  target_compile_definitions(${target} PRIVATE _GNU_SOURCE)
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

pftrace_add_library("" pf_ring_mmap.c "" "")

if(PFTRACE_BUILD_BENCH)
  pftrace_add_library(_mpsc pf_ring_mpsc.c PF_RING_BACKEND_MPSC ringbuf_mpsc)
  pftrace_add_library(_zcopy pf_ring_zcopy.c PF_RING_BACKEND_ZCOPY
                      ringbuf_zcopy)
endif()
