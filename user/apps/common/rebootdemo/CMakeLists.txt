# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Reboot-to-bootloader witness, and the unattended-reflash affordance (flash without a
# button press). Diagnostic app: kos_reboot is compiled out of the production ABI.
# Board-agnostic -- on a chip with an arch_reboot backend it hands the board over, and
# everywhere else the fallback declines, which is what the gates below assert.
kickos_add_diagnostic_app(rebootdemo
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET rebootdemo)
  return() # diagnostics off -> app not built; skip its tests
endif()

if(NOT KICKOS_BUILD_TESTS)
  return()
endif()

# rc=-38 is -KOS_ENOSYS (system/include/kickos/sys/errno.h). Registered only where the
# seam declines: a board with a real backend reboots instead of printing anything.
if(KICKOS_ARCH STREQUAL "sim")
  # The script, not PASS_REGULAR_EXPRESSION: a regex property makes CTest ignore the
  # exit status, so the refusal line plus a dirty exit would still pass. It runs
  # NATIVELY unless QEMU_MACHINE is set (same shape as check_fault_dump.sh).
  add_test(NAME sim_reboot_declined
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_reboot.sh" "$<TARGET_FILE:rebootdemo>")
  set_tests_properties(sim_reboot_declined PROPERTIES TIMEOUT 20)
elseif(KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_reboot_declined TARGET rebootdemo
    BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_reboot.sh" TIMEOUT 40)
elseif(KICKOS_BOARD STREQUAL "qemu-riscv")
  kickos_add_qemu_test(NAME qemu_riscv_reboot_declined TARGET rebootdemo BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_reboot.sh" TIMEOUT 40)
endif()
