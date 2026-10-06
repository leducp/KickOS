# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# strerror.cc carries prose no other member needs, so it is its own member: an image
# that never names an error code leaves it unextracted, as rtt.cc.obj already is.
add_library(kickos_lib STATIC
  libc/fmt.cc
  strerror.cc
  rtt.cc)
kickos_apply_freestanding(kickos_lib)
# errno.h / strerror.h live in the kickos_system header home.
target_include_directories(kickos_lib PRIVATE "${PROJECT_SOURCE_DIR}/system/include")

# Only where the kernel calls these under the app's names (kickos/kruntime.h). An object, never
# an archive member: beside the libc's archive, the link order would choose the copy.
# kickos_string_scan holds the same objects for the gates that read archives.
if(NOT KICKOS_ARCH STREQUAL "sim" AND NOT KICKOS_HAVE_ASPACE)
  add_library(kickos_string OBJECT libc/string.cc)
  kickos_apply_freestanding(kickos_string)
  target_include_directories(kickos_string PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  # The word loops in string.cc are the memcpy/memset idiom, and the pass that rewrites
  # that idiom into a library call would turn each of them into a call to itself.
  set_source_files_properties(libc/string.cc PROPERTIES
    COMPILE_OPTIONS "-fno-tree-loop-distribute-patterns")
  add_library(kickos_string_scan STATIC $<TARGET_OBJECTS:kickos_string>)
  kickos_export_targets(kickos_string)
endif()

# The ch1 telemetry ring writer (kickos_rtt_write_record_ch1) is called from the
# ARM PendSV tail via the switch hook, so it too must be FP-register-free there.
if(KICKOS_TELEMETRY_ON AND (KICKOS_ARCH STREQUAL "armv7m" OR KICKOS_ARCH STREQUAL "armv6m"))
  set_source_files_properties(rtt.cc PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
endif()
target_include_directories(kickos_lib PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
kickos_export_targets(kickos_lib)
