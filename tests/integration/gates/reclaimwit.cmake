# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The console-reclaim gates, one per mode, riding `reclaimwit` and `reclaimwit_drain`.

if(NOT TARGET reclaimwit)
  return()
endif()

# The service list must be kickos_services_none or the app refuses at run time, so the gate
# would fail by construction instead of witnessing anything.
if(NOT KICKOS_SERVICE_LIST STREQUAL "kickos_services_none")
  return()
endif()

set(_reclaimwit_script "${PROJECT_SOURCE_DIR}/tests/integration/check_reclaimwit.sh")

if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME sim_reclaimwit_park
    COMMAND "${_reclaimwit_script}" "$<TARGET_FILE:reclaimwit>" park)
  add_test(NAME sim_reclaimwit_drain
    COMMAND "${_reclaimwit_script}" "$<TARGET_FILE:reclaimwit_drain>" drain)
  set_tests_properties(sim_reclaimwit_park sim_reclaimwit_drain PROPERTIES TIMEOUT 120)
# Not armv8a and not x86_64: those build the app and register no arm today.
elseif(NOT KICKOS_ARCH STREQUAL "armv8a" AND NOT KICKOS_ARCH STREQUAL "x86_64")
  # The park arm is polled and killed, so its TIMEOUT must clear the poll bound
  # (QEMU_TIMEOUT * 5 polls at 0.2 s) rather than the boot.
  kickos_add_qemu_test(NAME ${_tag}_reclaimwit_park TARGET reclaimwit TIMEOUT 120
    SCRIPT "${_reclaimwit_script}" ARGS park)
  kickos_add_qemu_test(NAME ${_tag}_reclaimwit_drain TARGET reclaimwit_drain TIMEOUT 120
    SCRIPT "${_reclaimwit_script}" ARGS drain)
endif()
