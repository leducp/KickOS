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
add_executable(c6lpprobe main.cc "${_lp_probe_obj}")
target_link_libraries(c6lpprobe PRIVATE KickOS::kernel KickOS::system_default)
kickos_emit_image(c6lpprobe)
kickos_app_judge(c6lpprobe tests/integration/check_c6lpprobe.sh)
