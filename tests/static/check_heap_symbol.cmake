# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# kickos_heap_defsym's answers, run under cmake -P:
#   cmake -DKICKOS_SOURCE_DIR=<repo root> -P tests/static/check_heap_symbol.cmake

cmake_minimum_required(VERSION 3.24)
include("${KICKOS_SOURCE_DIR}/cmake/heap_symbol.cmake")

foreach(_case "4096=--defsym=KICKOS_USER_HEAP_SIZE=4096"
              "0x10000=--defsym=KICKOS_USER_HEAP_SIZE=0x10000" "0=--defsym=KICKOS_USER_HEAP_SIZE=0")
  string(FIND "${_case}" "=" _at)
  string(SUBSTRING "${_case}" 0 ${_at} _value)
  math(EXPR _at "${_at} + 1")
  string(SUBSTRING "${_case}" ${_at} -1 _want)
  kickos_heap_defsym(_got "${_value}")
  if(NOT _got STREQUAL _want)
    message(FATAL_ERROR "kickos_heap_defsym('${_value}') answered '${_got}', not '${_want}'")
  endif()
endforeach()
message(STATUS "heap_symbol: OK")
