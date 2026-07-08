# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_application(sched_exit
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

# The bug was ARM-only (deferred PendSV switch); the sim gate locks parity.
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_test(NAME sim_sched_exit COMMAND "$<TARGET_FILE:sched_exit>")
  set_tests_properties(sim_sched_exit PROPERTIES
    TIMEOUT 15
    PASS_REGULAR_EXPRESSION "root: survived worker exit"
    FAIL_REGULAR_EXPRESSION "panic|unreachable")
endif()

# QEMU armv7m gate: this is where the bug lived (deferred PendSV under IrqLock).
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu")
  add_test(NAME qemu_sched_exit
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_qemu_exit.sh" "$<TARGET_FILE:sched_exit>")
  set_tests_properties(qemu_sched_exit PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()

# QEMU armv6m gate: same deferred-switch mechanism on Cortex-M0.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "microbit")
  add_test(NAME microbit_sched_exit
    COMMAND "${CMAKE_COMMAND}" -E env QEMU_MACHINE=microbit
            "${PROJECT_SOURCE_DIR}/tests/check_qemu_exit.sh" "$<TARGET_FILE:sched_exit>")
  set_tests_properties(microbit_sched_exit PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()

# QEMU rv32imac gate: the same exit path over the RISC-V msip deferred switch.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv")
  add_test(NAME qemu_riscv_sched_exit
    COMMAND "${CMAKE_COMMAND}" -E env QEMU=qemu-system-riscv32 QEMU_MACHINE=virt "QEMU_EXTRA=-bios none"
            "${PROJECT_SOURCE_DIR}/tests/check_qemu_exit.sh" "$<TARGET_FILE:sched_exit>")
  set_tests_properties(qemu_riscv_sched_exit PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()
