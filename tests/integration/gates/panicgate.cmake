# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The panic-reporting arms riding the six `panicgate` images, one case per image.

if(NOT TARGET panicgate1)
  return()
endif()

# Two spellings of each verdict: the CTest regex (sim) and the literal the QEMU script greps
# with -F. Keep them in step.
set(_pg_re_1 "KERNEL PANIC: \\[panicgate\\] message on the wire")
set(_pg_txt_1 "KERNEL PANIC: [panicgate] message on the wire")
# Cases 2 and 3 carry the text the KERNEL supplies: diag.h's kUserPanicNoMsg.
set(_pg_re_2 "KERNEL PANIC: user panic \\(no readable message\\)")
set(_pg_txt_2 "KERNEL PANIC: user panic (no readable message)")
set(_pg_re_3 "${_pg_re_2}")
set(_pg_txt_3 "${_pg_txt_2}")
# Case 4 ends in the literal truncation marker, so the dots are escaped in the regex:
# unescaped they would match any three bytes and pass with no marker at all.
set(_pg_re_4 "KERNEL PANIC: \\[panicgate\\] abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUV\\.\\.\\.")
set(_pg_txt_4 "KERNEL PANIC: [panicgate] abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUV...")
# The tail the truncation must drop; passed as the script's third argument.
set(_pg_absent_4 "CUTME")
# Case 5: the four control bytes must each read back as '?' on ONE line. The expectation is
# the whole line: an unstripped message splits it, so the expected text never appears.
set(_pg_re_5 "KERNEL PANIC: \\[panicgate\\] ctl\\?\\?\\?\\? end")
set(_pg_txt_5 "KERNEL PANIC: [panicgate] ctl???? end")
# Case 6: main reads the status and runs on, the record carries the message, and nothing ends
# the system on the way.
set(_pg_re_6 "\\[panicgate\\] the task ended with KOS_EXIT_PANIC, the system runs on")
set(_pg_txt_6 "[panicgate] the task ended with KOS_EXIT_PANIC, the system runs on")
set(_pg_absent_6 "KERNEL PANIC:")
set(_pg_also_6 "[panicgate] task message")

foreach(_case 1 2 3 4 5 6)
  set(_pg_args "${_pg_txt_${_case}}")
  if(_pg_absent_${_case} OR _pg_also_${_case})
    list(APPEND _pg_args "${_pg_absent_${_case}}")
  endif()
  if(_pg_also_${_case})
    list(APPEND _pg_args "${_pg_also_${_case}}")
  endif()
  kickos_app_judge(panicgate${_case} tests/integration/check_qemu_panicgate.sh ARGS ${_pg_args})
  if(KICKOS_ARCH STREQUAL "sim")
    add_test(NAME panicgate${_case} COMMAND "$<TARGET_FILE:panicgate${_case}>")
    # PASS_REGULAR_EXPRESSION also makes CTest ignore the exit status, which
    # kfault_terminate sets to a fault code.
    set_tests_properties(panicgate${_case} PROPERTIES
      TIMEOUT 15
      PASS_REGULAR_EXPRESSION "${_pg_re_${_case}}"
      FAIL_REGULAR_EXPRESSION "\\[panicgate\\] ERROR;=== SIM FAULT;CUTME")
    if(_case EQUAL 6)
      set_tests_properties(panicgate6 PROPERTIES
        FAIL_REGULAR_EXPRESSION "\\[panicgate\\] ERROR;=== SIM FAULT;KERNEL PANIC")
    endif()
  else()
    # Every board with an emulator takes this one call, under the derived
    # <board tag>_panicgate<case> name: the script, the expected text and the absent tail are
    # the same on all ten, so no board branch survives here.
    kickos_add_qemu_test(TARGET panicgate${_case}
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_panicgate.sh"
      ARGS "${_pg_txt_${_case}}" "${_pg_absent_${_case}}" "${_pg_also_${_case}}")
  endif()
endforeach()
