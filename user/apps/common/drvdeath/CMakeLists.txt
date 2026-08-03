# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Console reclaim on driver death. Diagnostic app (KICKOS_ENABLE_SELFTEST only); sim-only
# because the published posture plus a driver that exits on cue is only automatable on
# the host (see tests/check_sim_drvdeath.sh).
if(NOT KICKOS_ARCH STREQUAL "sim")
  return()
endif()

kickos_add_diagnostic_app(drvdeath
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET drvdeath)
  return() # diagnostics off -> not built; skip the test
endif()

# Case 3 of the gate: the console driver is TWO threads and the second one holds the
# register window. The same knob the service list reads (system/CMakeLists.txt) -- the app
# needs it because the deferred-reclaim assertion and the cancel that ends it live here.
if(KICKOS_SIMCON_WINDOW_THREAD)
  target_compile_definitions(drvdeath PRIVATE KICKOS_SIMCON_WINDOW_THREAD=1)
endif()

# The gate builds its OWN tree (one service-list provider links per image, and this one
# also needs the bounded-serve knob), so register the test only in the ordinary tree --
# registering it in the tree the script configures would recurse.
if(KICKOS_BUILD_TESTS AND KICKOS_SERVICE_LIST STREQUAL "kickos_services_none")
  add_test(
    NAME    sim_driver_death
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_sim_drvdeath.sh"
            "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
  set_tests_properties(sim_driver_death PROPERTIES TIMEOUT 300)
endif()
