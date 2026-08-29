# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

if(NOT KICKOS_HAVE_LIBC)
  return()
endif()

kickos_add_diagnostic_app(mpu_fault
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET mpu_fault)
  return()
endif()

# Through the same script as the QEMU boards: a PASS/FAIL_REGULAR_EXPRESSION pair cannot
# require the control marker AND pin the fault address the app announced, and without both a
# total grant failure passes.
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_test(NAME mpu_fault
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_mpu_fault.sh" "$<TARGET_FILE:mpu_fault>"
            ${KICKOS_FAULT_OUTCOME})
  set_tests_properties(mpu_fault PROPERTIES TIMEOUT 15)
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv" AND KICKOS_HAVE_MPU)
  kickos_add_qemu_test(NAME qemu_riscv_mpu_fault TARGET mpu_fault BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_mpu_fault.sh" ARGS ${KICKOS_FAULT_OUTCOME})
endif()

if(KICKOS_BUILD_TESTS AND KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS AND KICKOS_HAVE_MPU)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_mpu_fault TARGET mpu_fault BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_mpu_fault.sh" ARGS ${KICKOS_FAULT_OUTCOME})
endif()
