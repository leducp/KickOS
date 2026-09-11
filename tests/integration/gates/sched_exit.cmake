# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The scheduler exit-path gates riding `sched_exit`, plus the multi-instance gate that only
# needs a sim tree.

if(NOT TARGET sched_exit)
  return()
endif()

# The wait-until-last phase can only complete in an image whose service list spawns no driver
# threads: a driver never exits, so kernel().live never reaches 1 and root parks forever.
# main() skips that phase when SCHED_EXIT_SERVICE_THREADS is 1, which the app file derives from
# this same service list, and a gate registered against a skipped phase witnesses nothing.
if(KICKOS_SERVICE_LIST AND NOT KICKOS_SERVICE_LIST STREQUAL "kickos_services_none")
  return()
endif()

set(_sched_exit_script "${PROJECT_SOURCE_DIR}/tests/integration/check_sched_exit.sh")

# Through the same script as the QEMU boards: PASS_REGULAR_EXPRESSION is an OR, so it cannot
# require the worker markers too, and root survives an exit that never happened.
if(KICKOS_ARCH STREQUAL "sim")
  add_test(NAME sim_sched_exit
    COMMAND "${_sched_exit_script}" "$<TARGET_FILE:sched_exit>")
  set_tests_properties(sim_sched_exit PROPERTIES TIMEOUT 15)
endif()

# Registered only in the SINGLE-instance tree: the gate configures its own tree with the knob
# on and would otherwise recurse.
if(KICKOS_ARCH STREQUAL "sim" AND NOT KICKOS_MULTI_INSTANCE)
  add_test(NAME sim_multi_instance
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_multi_instance.sh"
            "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
  set_tests_properties(sim_multi_instance PROPERTIES TIMEOUT 300)
endif()

# Not armv8a: the arm64 boards build the app and register no arm today.
if(NOT KICKOS_ARCH STREQUAL "armv8a")
  kickos_add_qemu_test(TARGET sched_exit SCRIPT "${_sched_exit_script}")
endif()
