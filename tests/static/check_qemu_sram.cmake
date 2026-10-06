# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The SRAM a microbit build's QEMU runs carries is the RAM link region its chip file states, so
# an image is never booted on less or more SRAM than it was linked for. Run under cmake -P:
#   cmake -DKICKOS_BUILD=<microbit build directory> -P tests/static/check_qemu_sram.cmake

cmake_minimum_required(VERSION 3.24)
include("${KICKOS_BUILD}/generated/chip/chip.cmake")
if(NOT DEFINED KICKOS_CHIP_LINK_RAM_LENGTH)
  message(FATAL_ERROR "${KICKOS_BUILD}/generated/chip/chip.cmake states no RAM link region")
endif()
math(EXPR _want "${KICKOS_CHIP_LINK_RAM_LENGTH}" OUTPUT_FORMAT DECIMAL)
file(GLOB_RECURSE _ctest_files "${KICKOS_BUILD}/CTestTestfile.cmake")
set(_seen "")
foreach(_file IN LISTS _ctest_files)
  file(STRINGS "${_file}" _lines REGEX "nrf51-soc\\.sram-size=")
  foreach(_line IN LISTS _lines)
    string(REGEX MATCHALL "nrf51-soc\\.sram-size=[0-9]+" _sizes "${_line}")
    list(APPEND _seen ${_sizes})
  endforeach()
endforeach()
list(REMOVE_DUPLICATES _seen)
if(NOT _seen STREQUAL "nrf51-soc.sram-size=${_want}")
  message(FATAL_ERROR "the registered QEMU runs carry '${_seen}', not the chip file's RAM link "
                      "length, nrf51-soc.sram-size=${_want}")
endif()
message(STATUS "qemu_sram: every QEMU run carries nrf51-soc.sram-size=${_want}")
