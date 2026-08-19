# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# strerror.cc carries prose no other member needs, so it is its own member: an image
# that never names an error code leaves it unextracted, as rtt.cc.obj already is.
add_library(kickos_lib STATIC
  libc/string.cc
  libc/fmt.cc
  strerror.cc
  rtt.cc)
kickos_apply_freestanding(kickos_lib)
# The word loops in string.cc are the memcpy/memset idiom, and the pass that rewrites
# that idiom into a library call would turn each of them into a call to itself.
set_source_files_properties(libc/string.cc PROPERTIES
  COMPILE_OPTIONS "-fno-tree-loop-distribute-patterns")
# errno.h / strerror.h live in the kickos_system header home.
target_include_directories(kickos_lib PRIVATE "${PROJECT_SOURCE_DIR}/system/include")

# The ch1 telemetry ring writer (kickos_rtt_write_record_ch1) is called from the
# ARM PendSV tail via the switch hook, so it too must be FP-register-free there.
if(KICKOS_TELEMETRY_ON AND (KICKOS_ARCH STREQUAL "armv7m" OR KICKOS_ARCH STREQUAL "armv6m"))
  set_source_files_properties(rtt.cc PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
endif()
target_include_directories(kickos_lib PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
install(TARGETS kickos_lib EXPORT KickOSTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
