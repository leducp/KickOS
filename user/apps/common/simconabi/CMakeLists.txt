# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The console endpoint's framed arm against the sim's published driver. Sim-only: the
# driver it exercises is system/init/sim/service_list.cc, which is host-libc by
# construction (see tests/integration/check_sim_conabi.sh).
if(NOT KICKOS_ARCH STREQUAL "sim")
  return()
endif()

kickos_add_diagnostic_app(simconabi
  SOURCES main.cc "${PROJECT_SOURCE_DIR}/tests/tap/tap.cc"
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET simconabi)
  return() # diagnostics off -> not built; skip the test
endif()
target_include_directories(simconabi PRIVATE "${PROJECT_SOURCE_DIR}/tests/tap")

# The gate builds its OWN tree, one service-list provider linking per image, so register
# the test only in the ordinary tree. Registering it in the tree the script configures
# would recurse.
if(KICKOS_BUILD_TESTS AND KICKOS_SERVICE_LIST STREQUAL "kickos_services_none")
  add_test(
    NAME    sim_console_abi
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_conabi.sh"
            "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
  set_tests_properties(sim_console_abi PROPERTIES TIMEOUT 300)
endif()
