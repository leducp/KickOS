# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A console driver that prints at its start, restarted, riding `conrestart`.

if(NOT TARGET conrestart)
  return()
endif()

# The gate builds a tree of its own with a death knob of the console driver, so register it only
# in a tree that sets none; registering it in the tree the script configures would recurse.
if(KICKOS_SIMCON_EXIT_AFTER OR KICKOS_SIMCON_DIE_AT_BRINGUP OR KICKOS_SIMCON_WINDOW_THREAD
   OR KICKOS_SIMCON_IRQ_WEDGE)
  return()
endif()
add_test(
  NAME    sim_console_restart
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_conrestart.sh"
          "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
set_tests_properties(sim_console_restart PROPERTIES TIMEOUT 300)
