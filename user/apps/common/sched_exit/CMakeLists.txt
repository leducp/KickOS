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
  kickos_add_qemu_test(NAME qemu_sched_exit TARGET sched_exit BOARD qemu
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_exit.sh")
endif()

# QEMU armv6m gate: same deferred-switch mechanism on Cortex-M0.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "microbit")
  kickos_add_qemu_test(NAME microbit_sched_exit TARGET sched_exit BOARD microbit
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_exit.sh")
endif()

# QEMU rv32imac gate: the same exit path over the RISC-V msip deferred switch.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv")
  kickos_add_qemu_test(NAME qemu_riscv_sched_exit TARGET sched_exit BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_exit.sh")
endif()
