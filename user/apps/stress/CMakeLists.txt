# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_application(stress
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

# Bounded, self-verifying concurrency stress -- exercises scheduler + semaphores +
# the tickless timer under many mixed-priority threads, then asserts conservation.
# Sim gate: deterministic virtual time, so it is a reliable CI check. On MCUs it is
# a manual soak (real timing), run per board.
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_test(NAME sim_stress COMMAND "$<TARGET_FILE:stress>")
  set_tests_properties(sim_stress PROPERTIES
    TIMEOUT 60
    PASS_REGULAR_EXPRESSION "STRESS PASS"
    FAIL_REGULAR_EXPRESSION "STRESS FAIL|panic|unreachable")
endif()
