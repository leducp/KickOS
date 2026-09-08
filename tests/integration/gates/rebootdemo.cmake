# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The reboot-refusal gate riding `rebootdemo`. rc=-38 is -KOS_ENOSYS
# (system/include/kickos/sys/errno.h).

if(NOT TARGET rebootdemo)
  return()
endif()

set(_reboot_script "${PROJECT_SOURCE_DIR}/tests/integration/check_reboot.sh")

if(KICKOS_ARCH STREQUAL "sim")
  # The script, not PASS_REGULAR_EXPRESSION: a regex property makes CTest ignore the exit
  # status, so the refusal line plus a dirty exit would still pass.
  add_test(NAME sim_reboot_declined
    COMMAND "${_reboot_script}" "$<TARGET_FILE:rebootdemo>")
  set_tests_properties(sim_reboot_declined PROPERTIES TIMEOUT 20)
elseif(KICKOS_CHIP STREQUAL "mps2"
       OR KICKOS_BOARD STREQUAL "qemu-riscv"
       OR KICKOS_BOARD STREQUAL "qemu-riscv64")
  # The board list is enumerated rather than left to every board with an emulator: microbit and
  # the arm64 and x86_64 boards build the app and register no arm today.
  kickos_add_qemu_test(NAME ${_tag}_reboot_declined TARGET rebootdemo
    SCRIPT "${_reboot_script}" TIMEOUT 40)
endif()
