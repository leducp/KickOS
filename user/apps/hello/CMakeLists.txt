# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The in-tree consumer of the dependency-inversion helper: same 3-line shape a
# downstream project uses. Swapping BOARD sim -> an MCU keeps these sources.
kickos_add_application(hello
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_test(
    NAME    sim_demo
    COMMAND "${CMAKE_COMMAND}" -E env
            "${PROJECT_SOURCE_DIR}/tests/check_sim_demo.py"
            "$<TARGET_FILE:hello>")
  set_tests_properties(sim_demo PROPERTIES TIMEOUT 30)

  # Dependency-inversion acceptance: an out-of-tree app builds against the
  # exported KickOS package (find_package + kickos_add_application) and runs.
  add_test(
    NAME    oot_export
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_oot_export.sh"
            "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}"
            "${CMAKE_COMMAND}" "${CMAKE_GENERATOR}")
  set_tests_properties(oot_export PROPERTIES TIMEOUT 120)
endif()
