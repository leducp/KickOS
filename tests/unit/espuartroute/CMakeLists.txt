# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The ESP32 and ESP32-C6 device-interrupt pass over each chip's own UART_ROUTES
# (arch/xtensa/chip/esp32/uart_routes.h, arch/riscv/chip/esp32c6/uart_routes.h). Each chip's
# generated headers, chip_mmap.h forced ahead of the host build's own, reach its source alone.
set(_tool "${PROJECT_SOURCE_DIR}/tools/compose")
kickos_compose_inputs(_tool_inputs "${_tool}")
set(_sources "")
foreach(_pair "esp32;lx6;arch/xtensa/chip/esp32" "esp32c6;rv32imac;arch/riscv/chip/esp32c6")
  list(GET _pair 0 _chip)
  list(GET _pair 1 _arch)
  list(GET _pair 2 _dir)
  set(_gen "${CMAKE_CURRENT_BINARY_DIR}/${_chip}")
  set(_mmap "${_gen}/include/kickos/chip_mmap.h")
  kickos_compose_python(_python "${_tool}" "${_gen}/venv" "${_gen}/tmp")
  add_custom_command(
    OUTPUT "${_mmap}" "${_gen}/chip/irq.h"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_gen}/tmp"
    COMMAND ${_python} -m kickos_compose chip "${PROJECT_SOURCE_DIR}/platform/${_chip}/chip.yaml"
            --arch ${_arch} --include-dir "${_gen}/include" --chip-dir "${_gen}/chip"
    DEPENDS "${PROJECT_SOURCE_DIR}/platform/${_chip}/chip.yaml" ${_tool_inputs}
    COMMENT "Writing the ${_chip} chip headers for espuartroute"
    VERBATIM)
  set(_source "${CMAKE_CURRENT_SOURCE_DIR}/${_chip}.cc")
  set_source_files_properties("${_source}" PROPERTIES
    INCLUDE_DIRECTORIES "${PROJECT_SOURCE_DIR}/${_dir};${_gen}/chip;${PROJECT_SOURCE_DIR}/arch/common"
    COMPILE_OPTIONS "-include;${_mmap}"
    OBJECT_DEPENDS "${_mmap};${_gen}/chip/irq.h")
  list(APPEND _sources "${_source}" "${_mmap}")
  set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${_gen}")
endforeach()
kickos_add_unit_test(NAME espuartroute
  SOURCES ${_sources}
  INCLUDES "${PROJECT_SOURCE_DIR}/arch/common")
