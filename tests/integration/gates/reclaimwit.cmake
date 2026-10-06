# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The console-reclaim gates, one per mode, riding `reclaimwit` and `reclaimwit_drain`.

if(NOT TARGET reclaimwit)
  return()
endif()
kickos_app_judge(reclaimwit tests/integration/check_reclaimwit.sh ARGS park)
kickos_app_judge(reclaimwit_drain tests/integration/check_reclaimwit.sh ARGS drain)

set(_reclaimwit_script "${PROJECT_SOURCE_DIR}/tests/integration/check_reclaimwit.sh")

if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME sim_reclaimwit_park
    COMMAND "${_reclaimwit_script}" "$<TARGET_FILE:reclaimwit>" park)
  add_test(NAME sim_reclaimwit_drain
    COMMAND "${_reclaimwit_script}" "$<TARGET_FILE:reclaimwit_drain>" drain)
  set_tests_properties(sim_reclaimwit_park sim_reclaimwit_drain PROPERTIES TIMEOUT 120)
# Not armv8a and not x86_64: those build the app and register no arm today.
elseif(NOT KICKOS_ARCH STREQUAL "armv8a" AND NOT KICKOS_ARCH STREQUAL "x86_64")
  kickos_add_qemu_test(NAME ${_tag}_reclaimwit_park TARGET reclaimwit
    SCRIPT "${_reclaimwit_script}" ARGS park)
  kickos_add_qemu_test(NAME ${_tag}_reclaimwit_drain TARGET reclaimwit_drain
    SCRIPT "${_reclaimwit_script}" ARGS drain)
endif()
