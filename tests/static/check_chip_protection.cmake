# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# cmake/chip_generate.cmake's answers on a chip file's protection and on whether a board is
# described, run under cmake -P:
#   cmake -DKICKOS_SOURCE_DIR=<repo root> -DKICKOS_SCRATCH=<scratch directory>
#         -P tests/static/check_chip_protection.cmake

cmake_minimum_required(VERSION 3.24)
include("${KICKOS_SOURCE_DIR}/cmake/chip_generate.cmake")

# have_mpu, have_aspace, region unit, translates, and whether the pair disagrees.
foreach(_case "1;0;pmsav7;OFF;0" "1;0;;OFF;1" "0;0;;OFF;0" "0;1;;ON;0" "0;1;;OFF;1" "0;0;;ON;1" "1;0;pmp;ON;1"
              "0;0;pmp;OFF;0")
  list(GET _case 0 _mpu)
  list(GET _case 1 _aspace)
  list(GET _case 2 _unit)
  list(GET _case 3 _translates)
  list(GET _case 4 _want)
  kickos_chip_protection_disagreement(_said "${_mpu}" "${_aspace}" "${_unit}" "${_translates}" "chip.yaml")
  set(_got 0)
  if(NOT _said STREQUAL "")
    set(_got 1)
  endif()
  if(NOT _got EQUAL _want)
    message(FATAL_ERROR "kickos_chip_protection_disagreement(${_case}) answered '${_said}'")
  endif()
endforeach()

foreach(_case "pmsav6=PMSAV7=" "pmsav7=PMSAV7=" "sysmpu=SYSMPU=" "pmsav8=PMSAV8=arch_arm_pmsav8.cc")
  string(REPLACE "=" ";" _case "${_case}")
  list(GET _case 0 _unit)
  list(GET _case 1 _want)
  list(LENGTH _case _n)
  set(_want_source "")
  if(_n GREATER 2)
    list(GET _case 2 _want_source)
  endif()
  kickos_chip_arm_backend(_backend _source "${_unit}")
  get_filename_component(_name "${_source}" NAME)
  if(NOT _backend STREQUAL _want OR NOT "${_name}" STREQUAL "${_want_source}")
    message(FATAL_ERROR "kickos_chip_arm_backend(${_unit}) answered '${_backend}' '${_source}'")
  endif()
  if(NOT _source STREQUAL "" AND NOT EXISTS "${_source}")
    message(FATAL_ERROR "kickos_chip_arm_backend(${_unit}) names ${_source}, which does not exist")
  endif()
endforeach()

file(REMOVE_RECURSE "${KICKOS_SCRATCH}")
file(MAKE_DIRECTORY "${KICKOS_SCRATCH}/platform/part" "${KICKOS_SCRATCH}/boards/kit")
foreach(_case "chip.yaml;kit.yaml;composition.yaml;0" ";kit.yaml;composition.yaml;1" "chip.yaml;;composition.yaml;1"
              "chip.yaml;kit.yaml;;1")
  list(GET _case 0 _chip_file)
  list(GET _case 1 _board_file)
  list(GET _case 2 _default)
  list(GET _case 3 _want)
  file(REMOVE "${KICKOS_SCRATCH}/platform/part/chip.yaml" "${KICKOS_SCRATCH}/platform/part/kit.yaml"
              "${KICKOS_SCRATCH}/boards/kit/composition.yaml")
  foreach(_file "platform/part/${_chip_file}" "platform/part/${_board_file}" "boards/kit/${_default}")
    if(NOT _file MATCHES "/$")
      file(WRITE "${KICKOS_SCRATCH}/${_file}" "version: 1\n")
    endif()
  endforeach()
  kickos_board_undescribed(_said "${KICKOS_SCRATCH}" kit part)
  set(_got 0)
  if(NOT _said STREQUAL "")
    set(_got 1)
  endif()
  if(NOT _got EQUAL _want)
    message(FATAL_ERROR "kickos_board_undescribed(${_case}) answered '${_said}'")
  endif()
endforeach()
kickos_board_undescribed(_said "${KICKOS_SCRATCH}" kit "")
if(NOT _said STREQUAL "names no chip")
  message(FATAL_ERROR "kickos_board_undescribed answered '${_said}' on a board naming no chip")
endif()
message(STATUS "chip_protection: OK")
