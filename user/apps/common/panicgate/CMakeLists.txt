# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The kos_panic wire gate: one source, five images, because a run observes one panic.
# Diagnostic app (KICKOS_ENABLE_SELFTEST only). Board-agnostic: pure syscall, no MMIO,
# no arena spend.
#
# Every posture, MPU or not: case 3 turns on the kernel refusing to dereference an address
# the caller cannot reach, which the non-enforcing arm of arch_user_text_readable answers on
# every board's default posture.

# Two spellings of each verdict: the CTest regex (sim) and the literal the QEMU script
# greps with -F.
set(_pg_re_1 "KERNEL PANIC: \\[panicgate\\] message on the wire")
set(_pg_txt_1 "KERNEL PANIC: [panicgate] message on the wire")
# Cases 2 and 3 are the only ones whose text the KERNEL supplies, so they are the only
# ones KICKOS_DIAG_TERSE rewrites: kUserPanicNoMsg is P08 in diag.h.
if(KICKOS_DIAG_TERSE)
  set(_pg_re_2 "KERNEL PANIC: P08")
  set(_pg_txt_2 "KERNEL PANIC: P08")
else()
  set(_pg_re_2 "KERNEL PANIC: user panic \\(no readable message\\)")
  set(_pg_txt_2 "KERNEL PANIC: user panic (no readable message)")
endif()
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

foreach(_case 1 2 3 4 5)
  kickos_add_diagnostic_app(panicgate${_case}
    SOURCES main.cc
    BOARD   ${KICKOS_BOARD})
  if(NOT TARGET panicgate${_case})
    return() # diagnostics off -> not built; skip the tests
  endif()
  target_compile_definitions(panicgate${_case} PRIVATE KICKOS_PANICGATE_CASE=${_case})

  if(NOT KICKOS_BUILD_TESTS)
    continue()
  endif()
  if(KICKOS_ARCH STREQUAL "sim")
    add_test(NAME panicgate${_case} COMMAND "$<TARGET_FILE:panicgate${_case}>")
    # PASS_REGULAR_EXPRESSION also makes CTest ignore the exit status, which
    # kfault_terminate sets to a fault code.
    set_tests_properties(panicgate${_case} PROPERTIES
      TIMEOUT 15
      PASS_REGULAR_EXPRESSION "${_pg_re_${_case}}"
      FAIL_REGULAR_EXPRESSION "\\[panicgate\\] ERROR;=== SIM FAULT;CUTME")
  elseif(KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
    kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_panicgate${_case}
      TARGET panicgate${_case} BOARD ${KICKOS_BOARD}
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_panicgate.sh"
      ARGS "${_pg_txt_${_case}}" "${_pg_absent_${_case}}")
  elseif(KICKOS_BOARD STREQUAL "microbit")
    kickos_add_qemu_test(NAME microbit_panicgate${_case}
      TARGET panicgate${_case} BOARD microbit
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_panicgate.sh"
      ARGS "${_pg_txt_${_case}}" "${_pg_absent_${_case}}")
  elseif(KICKOS_BOARD STREQUAL "qemu-riscv")
    kickos_add_qemu_test(NAME qemu_riscv_panicgate${_case}
      TARGET panicgate${_case} BOARD qemu-riscv
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_panicgate.sh"
      ARGS "${_pg_txt_${_case}}" "${_pg_absent_${_case}}")
  elseif(KICKOS_BOARD STREQUAL "qemu-riscv64")
    kickos_add_qemu_test(NAME qemu_riscv64_panicgate${_case}
      TARGET panicgate${_case} BOARD qemu-riscv64
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_panicgate.sh"
      ARGS "${_pg_txt_${_case}}" "${_pg_absent_${_case}}")
  endif()
endforeach()
