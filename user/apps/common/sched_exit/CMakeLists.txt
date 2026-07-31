# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_application(sched_exit
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

# The bug was ARM-only (deferred PendSV switch); the sim gate locks parity. Through the
# same script as the QEMU boards: PASS_REGULAR_EXPRESSION is an OR, so it cannot require
# the worker markers TOO, and root survives an exit that never happened.
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_test(NAME sim_sched_exit
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_sched_exit.sh" "$<TARGET_FILE:sched_exit>")
  set_tests_properties(sim_sched_exit PROPERTIES TIMEOUT 15)
endif()

# QEMU armv7m gate: this is where the bug lived (deferred PendSV under IrqLock).
# Run on every MPS2 image: the deferred switch is the same code on each, but the
# exception-return path it rides differs per core.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_sched_exit TARGET sched_exit BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_sched_exit.sh")
endif()

# QEMU armv6m gate: same deferred-switch mechanism on Cortex-M0.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "microbit")
  kickos_add_qemu_test(NAME microbit_sched_exit TARGET sched_exit BOARD microbit
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_sched_exit.sh")
endif()

# QEMU rv32imac gate: the same exit path over the RISC-V msip deferred switch.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv")
  kickos_add_qemu_test(NAME qemu_riscv_sched_exit TARGET sched_exit BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_sched_exit.sh")
endif()
