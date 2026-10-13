# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The shutdown-outcome gate riding `rebootdemo`.

if(NOT TARGET rebootdemo)
  return()
endif()
kickos_emulator_judged(rebootdemo)

set(_reboot_script "${PROJECT_SOURCE_DIR}/tests/integration/check_reboot.sh")

if(KICKOS_ARCH STREQUAL "sim")
  # The script, not PASS_REGULAR_EXPRESSION: a regex property makes CTest ignore the exit
  # status, so the refusal line plus a dirty exit would still pass.
  add_test(NAME sim_reboot_declined
    COMMAND "${_reboot_script}" "$<TARGET_FILE:rebootdemo>" declined)
  kickos_boot_timeout(sim_reboot_declined "${_reboot_script}")
elseif(KICKOS_AMP_NODE AND KICKOS_AMP_OWN_IMAGE
       AND NOT KICKOS_AMP_NODE_ID EQUAL KICKOS_AMP_RESET_OWNER)
  kickos_add_qemu_test(NAME ${_tag}_reboot_refused TARGET rebootdemo
    SCRIPT "${_reboot_script}" ARGS refused)
else()
  # Every emulated machine models a reset.
  kickos_add_qemu_test(NAME ${_tag}_reboot_reset TARGET rebootdemo
    SCRIPT "${_reboot_script}" ARGS reset)
endif()
