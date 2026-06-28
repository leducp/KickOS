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
            "${PROJECT_SOURCE_DIR}/tests/check_hello.py"
            "$<TARGET_FILE:hello>")
  set_tests_properties(hello_demo PROPERTIES TIMEOUT 15)
endif()

# QEMU armv7m gate: boot the image on a QEMU Cortex-M4 and assert the userspace
# ping-pong runs -- end-to-end proof of the reset/scheduler/SVC/timer path.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu")
  add_test(
    NAME    qemu_hello
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_qemu_hello.sh" "$<TARGET_FILE:hello>")
  set_tests_properties(qemu_hello PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
endif()

# QEMU armv6m gate: same demo on a QEMU Cortex-M0 (BBC micro:bit / nRF51).
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "microbit")
  add_test(
    NAME    microbit_hello
    COMMAND "${CMAKE_COMMAND}" -E env QEMU_MACHINE=microbit
            "${PROJECT_SOURCE_DIR}/tests/check_qemu_hello.sh" "$<TARGET_FILE:hello>")
  set_tests_properties(microbit_hello PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
endif()
