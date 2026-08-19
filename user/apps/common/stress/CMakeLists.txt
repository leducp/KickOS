# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_application(stress
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

# Endurance soak: repeat the conservation round forever (heartbeat per round, freeze
# on the first FAIL). OFF keeps the single-round CTest-gate behaviour byte-identical.
option(KICKOS_STRESS_FOREVER "stress: loop the conservation round forever (soak)" OFF)
if(KICKOS_STRESS_FOREVER)
  target_compile_definitions(stress PRIVATE KICKOS_STRESS_FOREVER)
endif()

# Bounded, self-verifying concurrency stress: exercises scheduler + semaphores +
# the tickless timer under many mixed-priority threads, then asserts conservation.
# The sim has NO virtual clock: arch_clock_now reads CLOCK_MONOTONIC and the one-shot is a
# real timer_create delivering SIGALRM, so this gate is wall-clock timed like the MCU runs
# and its conservation counts, not its timings, are what it asserts. On MCUs it is a
# manual soak, run per board.
if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_test(NAME sim_stress COMMAND "$<TARGET_FILE:stress>")
  # KICKOS_PANIC_REGEX, not a lowercase "panic|unreachable": CTest's
  # FAIL_REGULAR_EXPRESSION is case-sensitive, and the kernel prints "KERNEL PANIC:",
  # so the old pattern matched only KICKOS_UNREACHABLE and let any panic AFTER the
  # pass marker ship green.
  set_tests_properties(sim_stress PROPERTIES
    TIMEOUT 60
    PASS_REGULAR_EXPRESSION "STRESS PASS"
    FAIL_REGULAR_EXPRESSION "STRESS FAIL;${KICKOS_PANIC_REGEX}")
endif()
