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
            "${CMAKE_CURRENT_SOURCE_DIR}/../../../tests/check_sim_demo.py"
            "$<TARGET_FILE:hello>")
  set_tests_properties(sim_demo PROPERTIES TIMEOUT 30)
endif()
