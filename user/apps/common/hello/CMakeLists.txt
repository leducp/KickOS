# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The friendly demo: the same 3-line consumer shape a downstream project uses.
kickos_add_application(hello
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

# Light smoke test: runs the endless demo briefly, Ctrl+C, checks it ping-ponged
# and halted cleanly.
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_test(
    NAME    hello_demo
    COMMAND "${CMAKE_COMMAND}" -E env
            "${PROJECT_SOURCE_DIR}/tests/integration/check_hello.py"
            "$<TARGET_FILE:hello>")
  set_tests_properties(hello_demo PROPERTIES TIMEOUT 15)
endif()

# QEMU armv7m gate: boot each MPS2 image and assert the userspace ping-pong runs,
# end-to-end proof of the reset/scheduler/SVC/timer path.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_hello TARGET hello BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_hello.sh")
endif()

# QEMU armv6m gate: same demo on a QEMU Cortex-M0 (BBC micro:bit / nRF51).
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "microbit")
  kickos_add_qemu_test(NAME microbit_hello TARGET hello BOARD microbit
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_hello.sh")
endif()

# QEMU RISC-V gate: the same demo on a QEMU `virt` (RV32IMAC, M-mode bare metal),
# end-to-end proof of the rv32imac reset/scheduler(msip)/ecall/CLINT-timer path.
# `-bios none` runs our image directly in M-mode (no OpenSBI).
# Registered on the FLAT variant only. Nothing in main.cc excludes enforcement: the players
# name both semaphores by DELEGATED CAP INDEX (ping@1, pong@2) and dereference nothing of
# main's. The enforcement arm is simply unwitnessed here.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv" AND NOT KICKOS_HAVE_MPU)
  kickos_add_qemu_test(NAME qemu_riscv_hello TARGET hello BOARD qemu-riscv
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_hello.sh")
endif()
