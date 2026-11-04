# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The LP core's silicon witness on the board's default composition. The parent builds it flat.
# The app helper injects C headers into every app TU, assembler sources included, so the LP reset
# stub is assembled on its own and those declarations never reach gas.
set(_lp_probe_obj "${CMAKE_CURRENT_BINARY_DIR}/lp_probe.o")
add_custom_command(OUTPUT "${_lp_probe_obj}"
  COMMAND "${CMAKE_C_COMPILER}" ${KICKOS_MCPU} -c
          "${CMAKE_CURRENT_SOURCE_DIR}/lp_probe.S" -o "${_lp_probe_obj}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/lp_probe.S"
  VERBATIM)
set_source_files_properties("${_lp_probe_obj}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
# The payload is the kernel's: the chip's bring-up copies it into LP RAM. The app names the
# bring-up's marker to pull it in, which only a board enforcing nothing lets an image do.
add_executable(c6lpprobe main.cc)
add_custom_target(c6lpprobe_payload DEPENDS "${_lp_probe_obj}")
add_dependencies(c6lpprobe c6lpprobe_payload)
set_target_properties(c6lpprobe PROPERTIES KICKOS_IMAGE_KERNEL_OBJECTS "${_lp_probe_obj}"
                                           KICKOS_IMAGE_KERNEL_NAMES kickos_c6_lp_marker)
set_property(TARGET c6lpprobe APPEND PROPERTY LINK_DEPENDS "${_lp_probe_obj}")
target_link_libraries(c6lpprobe PRIVATE KickOS::kickos KickOS::system_default)
kickos_emit_image(c6lpprobe)
kickos_app_judge(c6lpprobe tests/integration/check_c6lpprobe.sh)
