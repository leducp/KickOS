# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The console endpoint's framed arm, riding `simconabi` against the sim's published driver.

if(NOT TARGET simconabi)
  return()
endif()

# The gate builds its OWN tree, one service-list provider linking per image, so register
# the test only in the ordinary tree. Registering it in the tree the script configures
# would recurse.
if(KICKOS_SERVICE_LIST STREQUAL "kickos_services_none")
  add_test(
    NAME    sim_console_abi
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_conabi.sh"
            "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
  set_tests_properties(sim_console_abi PROPERTIES TIMEOUT 300)
endif()
