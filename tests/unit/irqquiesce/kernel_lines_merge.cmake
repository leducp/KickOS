# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# kickos_kernel_lines over hand lists: the doorbell half joins only where a doorbell rings, a lone
# line 0 is one line, and a malformed entry stops the configure.
#   cmake -DKICKOS_SOURCE_DIR=<repo root> -P tests/unit/irqquiesce/kernel_lines_merge.cmake

include("${KICKOS_SOURCE_DIR}/cmake/kernel_lines.cmake")

if(DEFINED CASE)
  if(CASE STREQUAL "malformed")
    kickos_kernel_lines(_out chip_m ON "TICK=7;BROKEN=" "")
  elseif(CASE STREQUAL "malformed_quiet_doorbell")
    kickos_kernel_lines(_out chip_q OFF "TICK=7" "BELL")
  endif()
  return()
endif()

function(expect what got want)
  if(NOT "${got}" STREQUAL "${want}")
    message(FATAL_ERROR "FAIL: ${what}: got `${got}`, want `${want}`")
  endif()
endfunction()

kickos_kernel_lines(_out chip OFF "TICK=0" "BELL=26")
expect("line 0, no doorbell" "${_out}" "0")
list(LENGTH _out _count)
expect("a lone line 0 is one line" "${_count}" "1")
kickos_kernel_lines(_out chip ON "TICK=0" "BELL=26")
expect("line 0, a doorbell ringing" "${_out}" "0;26")
kickos_kernel_lines(_out chip OFF "" "BELL=26")
expect("only a doorbell line, none ringing" "${_out}" "")
kickos_kernel_lines(_out chip ON "" "BELL=26")
expect("only a doorbell line, one ringing" "${_out}" "26")
kickos_kernel_lines(_out chip ON "" "")
expect("no line" "${_out}" "")

foreach(_case_chip IN ITEMS "malformed;chip_m" "malformed_quiet_doorbell;chip_q")
  list(GET _case_chip 0 _case)
  list(GET _case_chip 1 _chip)
  execute_process(COMMAND "${CMAKE_COMMAND}" -DKICKOS_SOURCE_DIR=${KICKOS_SOURCE_DIR}
                          -DCASE=${_case} -P "${CMAKE_CURRENT_LIST_FILE}"
                  RESULT_VARIABLE _rc ERROR_VARIABLE _err)
  if(_rc EQUAL 0)
    message(FATAL_ERROR "FAIL: ${_case} configured without a refusal")
  endif()
  string(FIND "${_err}" "chip `${_chip}`" _at)
  if(_at EQUAL -1)
    message(FATAL_ERROR "FAIL: ${_case} was refused without naming its chip:\n${_err}")
  endif()
endforeach()
message("PASS: kernel lines merge and refusals")
