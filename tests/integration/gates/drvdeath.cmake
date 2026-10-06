# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The console-reclaim-on-driver-death arm, riding `drvdeath`.

if(NOT TARGET drvdeath)
  return()
endif()

# The gate builds a tree of its own per death knob of the console driver, so register it only
# in a tree that sets none; registering it in the trees the script configures would recurse.
if(KICKOS_SIMCON_EXIT_AFTER OR KICKOS_SIMCON_DIE_AT_BRINGUP OR KICKOS_SIMCON_WINDOW_THREAD
   OR KICKOS_SIMCON_IRQ_WEDGE)
  return()
endif()
add_test(
  NAME    sim_driver_death
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_drvdeath.sh"
          "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
set_tests_properties(sim_driver_death PROPERTIES TIMEOUT 300)
