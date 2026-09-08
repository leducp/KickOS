# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The console-reclaim-on-driver-death arm, riding `drvdeath`.

if(NOT TARGET drvdeath)
  return()
endif()

# The gate builds its OWN tree (one service-list provider links per image, and this one
# also needs the bounded-serve knob), so register the test only in the ordinary tree;
# registering it in the tree the script configures would recurse.
if(KICKOS_SERVICE_LIST STREQUAL "kickos_services_none")
  add_test(
    NAME    sim_driver_death
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_drvdeath.sh"
            "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
  set_tests_properties(sim_driver_death PROPERTIES TIMEOUT 300)
endif()
