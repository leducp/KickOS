# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The ESP32-C6 partition gate's register image (arch/riscv/chip/esp32c6/apm_rows.h), programmed
# over a fake APM register file. The C6's chip_mmap.h, written from platform/esp32c6/chip.yaml, is
# forced ahead of every include: the host build's own chip_mmap.h is the sim's.
# Without uv the gate fails when built, not every configure that carries the unit tests.
find_program(KICKOS_UV uv)
set(_tool "${PROJECT_SOURCE_DIR}/tools/compose")
set(_chip "${PROJECT_SOURCE_DIR}/platform/esp32c6/chip.yaml")
set(_gen "${CMAKE_CURRENT_BINARY_DIR}/esp32c6")
set(_mmap "${_gen}/include/kickos/chip_mmap.h")
if(KICKOS_UV)
  set(_write
    COMMAND "${CMAKE_COMMAND}" -E env "UV_PROJECT_ENVIRONMENT=${_gen}/venv" UV_PYTHON_DOWNLOADS=never
            "PYTHONPATH=${_tool}" PYTHONDONTWRITEBYTECODE=1 "TMPDIR=${_gen}/tmp"
            "${KICKOS_UV}" run --project "${_tool}" --locked --quiet
            python -m kickos_compose chip "${_chip}" --arch rv32imac
            --include-dir "${_gen}/include" --chip-dir "${_gen}/chip")
else()
  set(_write
    COMMAND "${CMAKE_COMMAND}" -E echo "uv not found: the c6apm gate generates its chip header with uv"
    COMMAND "${CMAKE_COMMAND}" -E false)
endif()
file(GLOB _tool_sources CONFIGURE_DEPENDS "${_tool}/kickos_compose/*.py")
add_custom_command(
  OUTPUT "${_mmap}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_gen}/tmp"
  ${_write}
  DEPENDS "${_chip}" ${_tool_sources} "${_tool}/pyproject.toml" "${_tool}/uv.lock"
  COMMENT "Writing the ESP32-C6 chip headers for c6apm"
  VERBATIM)
# Everything the command writes beside the header, the chip sources, the venv and its scratch.
set_property(DIRECTORY APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${_gen}")
kickos_add_unit_test(NAME c6apm
  SOURCES c6apm.cc "${_mmap}"
  INCLUDES "${PROJECT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/user/include")
target_compile_options(c6apm PRIVATE -include "${_mmap}")
