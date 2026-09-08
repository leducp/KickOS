# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The libc exit-path gate riding `libc_exit`, through one script on the host and on every
# emulated board.

if(NOT TARGET libc_exit)
  return()
endif()

set(_libc_exit_script "${PROJECT_SOURCE_DIR}/tests/integration/check_libc_exit.sh")

if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME sim_libc_exit
    COMMAND "${_libc_exit_script}" "$<TARGET_FILE:libc_exit>")
  set_tests_properties(sim_libc_exit PROPERTIES TIMEOUT 15)
endif()

# The board list is enumerated rather than left to every board with an emulator: the arm64 and
# x86_64 boards build the app and register no arm today.
if(KICKOS_CHIP STREQUAL "mps2"
   OR KICKOS_BOARD STREQUAL "microbit"
   OR KICKOS_BOARD STREQUAL "qemu-riscv"
   OR KICKOS_BOARD STREQUAL "qemu-riscv64")
  kickos_add_qemu_test(TARGET libc_exit SCRIPT "${_libc_exit_script}")
endif()
