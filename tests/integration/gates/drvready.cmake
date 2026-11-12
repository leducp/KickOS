# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The packaged driver's startup barrier, riding `uartloop` over simuart.

if(NOT TARGET uartloop)
  return()
endif()

# The gate builds a tree of its own per knob of simuart, so register it only in the sim preset's
# tree, which sets none; registering it in the trees the script configures would recurse.
if(NOT _fs_preset_key STREQUAL "sim"
   OR (DEFINED KICKOS_SIMUART_READY AND NOT KICKOS_SIMUART_READY STREQUAL ""))
  return()
endif()
add_test(
  NAME    sim_driver_ready
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_drvready.sh"
          "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
set_tests_properties(sim_driver_ready PROPERTIES TIMEOUT 300)
