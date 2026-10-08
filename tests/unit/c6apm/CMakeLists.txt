# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The ESP32-C6 partition gate's register image (arch/riscv/chip/esp32c6/apm_rows.h), programmed
# over a fake APM register file. The C6's chip_mmap.h, written from platform/esp32c6/chip.yaml, is
# forced ahead of every include: the host build's own chip_mmap.h is the sim's.
set(_tool "${PROJECT_SOURCE_DIR}/tools/compose")
set(_chip "${PROJECT_SOURCE_DIR}/platform/esp32c6/chip.yaml")
set(_gen "${CMAKE_CURRENT_BINARY_DIR}/esp32c6")
set(_mmap "${_gen}/include/kickos/chip_mmap.h")
kickos_compose_python(_python "${_tool}" "${_gen}/venv" "${_gen}/tmp")
kickos_compose_inputs(_tool_inputs "${_tool}")
add_custom_command(
  OUTPUT "${_mmap}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_gen}/tmp"
  COMMAND ${_python} -m kickos_compose chip "${_chip}" --arch rv32imac
          --include-dir "${_gen}/include" --chip-dir "${_gen}/chip"
  DEPENDS "${_chip}" ${_tool_inputs}
  COMMENT "Writing the ESP32-C6 chip headers for c6apm"
  VERBATIM)
# Everything the command writes beside the header, the chip sources, the venv and its scratch.
set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${_gen}")
kickos_add_unit_test(NAME c6apm
  SOURCES c6apm.cc "${_mmap}"
  INCLUDES "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/user/include")
target_compile_options(c6apm PRIVATE -include "${_mmap}")
