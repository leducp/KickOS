# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_library(kickos_lib STATIC
  libc/string.cc
  libc/fmt.cc
  rtt.cc)
kickos_apply_freestanding(kickos_lib)

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
