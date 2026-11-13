# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# userbench's rows against the posture it states.

if(NOT TARGET userbench)
  return()
endif()
kickos_app_judge(userbench tests/integration/check_userbench.sh)
if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME sim_userbench
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_userbench.sh" "$<TARGET_FILE:userbench>")
  kickos_boot_timeout(sim_userbench "${PROJECT_SOURCE_DIR}/tests/integration/check_userbench.sh")
endif()
kickos_add_qemu_test(TARGET userbench
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_userbench.sh")
