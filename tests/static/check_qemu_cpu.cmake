# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# kickos_qemu_cpu_line and kickos_qemu_cpu_model (cmake/qemu_cpu.cmake), run under cmake -P:
#   cmake -DKICKOS_SOURCE_DIR=<repo root> -P tests/static/check_qemu_cpu.cmake
# The refusal is a child run of this file with CASE set, which must fail naming its token.

cmake_minimum_required(VERSION 3.24)
include("${KICKOS_SOURCE_DIR}/cmake/qemu_cpu.cmake")

if(DEFINED CASE)
  if(CASE STREQUAL "no_slot")
    # imx8mp-evk's line: its machine fixes the core.
    kickos_qemu_cpu_line(_line cortex-a710 imx8mp-evk -m 512M)
  endif()
  message(STATUS "ACCEPTED: ${_line}")
  return()
endif()

file(STRINGS "${KICKOS_SOURCE_DIR}/cmake/kickos.cmake" _calls
     REGEX "^  kickos_qemu_cpu_(line|model)\\(")
list(LENGTH _calls _n)
if(NOT _n EQUAL 2)
  message(FATAL_ERROR "FAIL: cmake/kickos.cmake calls kickos_qemu_cpu_line and "
    "kickos_qemu_cpu_model ${_n} times in all, not once each, so the arms below may test what "
    "no configure runs")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" -DKICKOS_SOURCE_DIR=${KICKOS_SOURCE_DIR}
                        -DCASE=no_slot -P "${CMAKE_CURRENT_LIST_FILE}"
                RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(_rc EQUAL 0)
  message(FATAL_ERROR "FAIL: a processor for a line naming no -cpu was accepted: ${_out}")
endif()
string(FIND "${_err}" "QEMU_CPU_NO_SLOT" _at)
if(_at EQUAL -1)
  message(FATAL_ERROR "FAIL: a line naming no -cpu was refused, but not for its slot:\n${_err}")
endif()

function(expect what got want)
  if(NOT "${got}" STREQUAL "${want}")
    message(FATAL_ERROR "FAIL: ${what}: got '${got}', want '${want}'")
  endif()
endfunction()

kickos_qemu_cpu_line(_line cortex-a710 qemu-arm64 -cpu cortex-a53 -m 1G)
expect("the processor replaced in place" "${_line}" "-cpu;cortex-a710;-m;1G")
kickos_qemu_cpu_line(_line "" imx8mp-evk -m 512M)
expect("no processor asked keeps a line naming none" "${_line}" "-m;512M")

set(_a53 "-mcpu=cortex-a53;-mgeneral-regs-only")
kickos_qemu_cpu_model(_cpu "" "${_a53}" -m 512M)
expect("a line naming no -cpu runs the chip's core" "${_cpu}" "cortex-a53")
kickos_qemu_cpu_model(_cpu "" "${_a53}" -cpu max,pauth=off -m 1G)
expect("the line's -cpu, without its properties" "${_cpu}" "max")
kickos_qemu_cpu_model(_cpu cortex-a710 "${_a53}" -cpu cortex-a53)
expect("the preset's processor" "${_cpu}" "cortex-a710")
kickos_qemu_cpu_model(_cpu "" "-mcpu=cortex-a53+crc" -m 512M)
expect("the chip's core, without its extensions" "${_cpu}" "cortex-a53")
kickos_qemu_cpu_model(_cpu "" "-march=armv8-a" -m 512M)
expect("nothing names a core" "${_cpu}" "")
message("PASS: the emulated processor and its replacement")
