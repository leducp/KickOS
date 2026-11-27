# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# userbench's rows against the posture it states.

if(NOT TARGET userbench)
  return()
endif()
get_target_property(_userbench_flash_text userbench USERBENCH_FLASH_TEXT)
set(_userbench_judge_args "")
if(_userbench_flash_text)
  set(_userbench_judge_args ARGS flash)
endif()
# Its run outlasts the default capture window on the slowest boards (picopi).
kickos_app_judge(userbench tests/integration/check_userbench.sh ${_userbench_judge_args} WINDOW 90)
if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME sim_userbench
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_userbench.sh" "$<TARGET_FILE:userbench>")
  kickos_boot_timeout(sim_userbench "${PROJECT_SOURCE_DIR}/tests/integration/check_userbench.sh")
endif()
kickos_add_qemu_test(TARGET userbench
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_userbench.sh")
