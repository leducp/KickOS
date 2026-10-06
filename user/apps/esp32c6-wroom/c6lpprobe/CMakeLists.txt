# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

if(NOT KICKOS_HAVE_MPU)
  # The app helper injects C headers for every app TU, including assembler sources.
  # Assemble this reset stub separately so those C declarations never reach gas.
  set(_lp_probe_obj "${CMAKE_CURRENT_BINARY_DIR}/lp_probe.o")
  add_custom_command(OUTPUT "${_lp_probe_obj}"
    COMMAND "${CMAKE_C_COMPILER}" ${KICKOS_MCPU} -c
            "${CMAKE_CURRENT_SOURCE_DIR}/lp_probe.S" -o "${_lp_probe_obj}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/lp_probe.S"
    VERBATIM)
  set_source_files_properties("${_lp_probe_obj}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
  add_executable(c6lpprobe main.cc "${_lp_probe_obj}")
  target_link_libraries(c6lpprobe PRIVATE kickos)
  kickos_emit_image(c6lpprobe)
endif()
