# The library itself.
#
# Sources are listed relative to this directory, so the paths stay correct
# if the directory is ever vendored into another project.
add_library(pftrace STATIC
  pb_writer.c
  pf_ring.c
  pftrace.c
)

# include/ is PUBLIC (consumers need the API header); this directory is
# PRIVATE, holding pf_ring.h and pb_writer.h, which are implementation
# details. tests/test_ring.c reaches in here deliberately -- see its own
# CMakeLists.txt.
target_include_directories(pftrace
  PUBLIC ${PROJECT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(pftrace PUBLIC Threads::Threads)

# memfd_create (the ring's double mapping) is glibc's, not ISO C's, and
# unlike clock_gettime/CLOCK_BOOTTIME it is not exposed by _DEFAULT_SOURCE --
# only _GNU_SOURCE declares it. Without this the build fails on an implicit
# declaration. PRIVATE: the public header is self-contained and needs no
# feature macro from consumers.
target_compile_definitions(pftrace PRIVATE _GNU_SOURCE)

set_target_properties(pftrace PROPERTIES
  C_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)
