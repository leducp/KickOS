# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gates riding `mpu_fault`: a grant violation, and the address the app announced.

if(NOT TARGET mpu_fault)
  return()
endif()

# Through the same script as the QEMU boards: a PASS/FAIL_REGULAR_EXPRESSION pair cannot
# require the control marker AND pin the fault address the app announced, and without both a
# total grant failure passes.
if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME mpu_fault
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_mpu_fault.sh" "$<TARGET_FILE:mpu_fault>"
            ${KICKOS_FAULT_OUTCOME})
  set_tests_properties(mpu_fault PROPERTIES TIMEOUT 15)
endif()

# The two emulator postures that carry an MPU take the identical call, under the derived
# <board tag>_mpu_fault name.
if(KICKOS_HAVE_MPU AND (KICKOS_BOARD STREQUAL "qemu-riscv" OR KICKOS_CHIP STREQUAL "mps2"))
  kickos_add_qemu_test(TARGET mpu_fault
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_mpu_fault.sh"
    ARGS ${KICKOS_FAULT_OUTCOME})
endif()
