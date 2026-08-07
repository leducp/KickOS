# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The ROOT-confinement gate, in its own binary (it ends the process on the deliberate
# cross-domain fault when root is confined). Diagnostic app: built only under
# KICKOS_ENABLE_SELFTEST. Board-agnostic (pure syscall plus kos_ram_alloc).
kickos_add_diagnostic_app(rootfault
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET rootfault)
  return() # diagnostics off -> app not built; skip its tests
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_HAVE_MPU)
  # Through the same script as the QEMU boards: a PASS/FAIL_REGULAR_EXPRESSION pair
  # cannot require the child's control marker AND pin the fault address to the region
  # root announced.
  if(KICKOS_ARCH STREQUAL "sim")
    add_test(NAME rootfault
      COMMAND "${PROJECT_SOURCE_DIR}/tests/check_rootfault.sh" "$<TARGET_FILE:rootfault>"
              ${KICKOS_FAULT_OUTCOME})
    set_tests_properties(rootfault PROPERTIES TIMEOUT 15)
  elseif(KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
    kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_rootfault TARGET rootfault
      BOARD ${KICKOS_BOARD}
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_rootfault.sh" ARGS ${KICKOS_FAULT_OUTCOME})
  elseif(KICKOS_BOARD STREQUAL "qemu-riscv")
    kickos_add_qemu_test(NAME qemu_riscv_rootfault TARGET rootfault BOARD qemu-riscv
      SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_rootfault.sh" ARGS ${KICKOS_FAULT_OUTCOME})
  endif()
endif()
