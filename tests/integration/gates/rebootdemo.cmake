# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The reboot-refusal gate riding `rebootdemo`. rc=-38 is -KOS_ENOSYS
# (system/include/kickos/sys/errno.h).

if(NOT TARGET rebootdemo)
  return()
endif()
kickos_emulator_judged(rebootdemo)

set(_reboot_script "${PROJECT_SOURCE_DIR}/tests/integration/check_reboot.sh")

if(KICKOS_ARCH STREQUAL "sim")
  # The script, not PASS_REGULAR_EXPRESSION: a regex property makes CTest ignore the exit
  # status, so the refusal line plus a dirty exit would still pass.
  add_test(NAME sim_reboot_declined
    COMMAND "${_reboot_script}" "$<TARGET_FILE:rebootdemo>")
  kickos_boot_timeout(sim_reboot_declined "${_reboot_script}")
else()
  kickos_add_qemu_test(NAME ${_tag}_reboot_declined TARGET rebootdemo
    SCRIPT "${_reboot_script}")
endif()
