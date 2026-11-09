# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# mutexbench's rows against the load its THREADS budget seats.

if(NOT TARGET mutexbench)
  return()
endif()
set(_posture "")
if(KICKOS_HAVE_ASPACE)
  list(APPEND _posture aspace)
endif()
kickos_app_judge(mutexbench tests/integration/check_mutexbench.sh ARGS ${_posture})
if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME sim_mutexbench
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_mutexbench.sh" "$<TARGET_FILE:mutexbench>"
            ${_posture})
  kickos_boot_timeout(sim_mutexbench "${PROJECT_SOURCE_DIR}/tests/integration/check_mutexbench.sh")
endif()
kickos_add_qemu_test(TARGET mutexbench
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_mutexbench.sh" ARGS ${_posture})
