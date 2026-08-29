# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The ROOT-narrow gate. Diagnostic app: built only under KICKOS_ENABLE_SELFTEST.
# Board-agnostic: pure syscall, no MMIO. One 16-byte kos_ram_alloc (the AUTH_MEMORY arm).
kickos_add_diagnostic_app(rootauth
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET rootauth)
  return() # diagnostics off -> app not built; skip its tests
endif()

# EXACTLY five arms, all unconditional (main.cc), on every board: the count must equal
# that, because any slack lets an arm be deleted with every gate still green and the arm
# most worth deleting is the post-narrow refusal, the only one separating a narrow that
# took effect from one that returned 0 and changed nothing. Raise this with the arm count.
set(_arms 5)
# main prints PASS and then returns, and that return calls kos_shutdown (gated on
# KOS_AUTH_SYSTEM), so a regression dropping only that bit panics AFTER the verdict.
set(_absent "root: shutdown refused")

# No KICKOS_HAVE_MPU condition: authority is a capability check, not memory enforcement.
if(KICKOS_BUILD_TESTS)
  if(KICKOS_ARCH STREQUAL "sim")
    add_test(NAME rootauth
      COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_app_arms.sh" "$<TARGET_FILE:rootauth>"
              rootauth ${_arms} "${_absent}")
    set_tests_properties(rootauth PROPERTIES TIMEOUT 15)
  elseif(KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
    kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_rootauth TARGET rootauth
      BOARD ${KICKOS_BOARD}
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_app_arms.sh"
      ARGS rootauth ${_arms} "${_absent}")
  elseif(KICKOS_BOARD STREQUAL "microbit")
    kickos_add_qemu_test(NAME microbit_rootauth TARGET rootauth BOARD microbit
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_app_arms.sh"
      ARGS rootauth ${_arms} "${_absent}")
  elseif(KICKOS_BOARD STREQUAL "qemu-riscv")
    kickos_add_qemu_test(NAME qemu_riscv_rootauth TARGET rootauth BOARD qemu-riscv
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_app_arms.sh"
      ARGS rootauth ${_arms} "${_absent}")
  elseif(KICKOS_BOARD STREQUAL "qemu-riscv64")
    kickos_add_qemu_test(NAME qemu_riscv64_rootauth TARGET rootauth BOARD qemu-riscv64
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_app_arms.sh"
      ARGS rootauth ${_arms} "${_absent}")
  endif()
endif()
