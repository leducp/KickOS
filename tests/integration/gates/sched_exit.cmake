# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The multi-instance gate, whose payload is `sched_exit`.

if(NOT TARGET sched_exit)
  return()
endif()
kickos_unbooted(sched_exit "sim_multi_instance boots it in a tree of its own")

# The gate configures the sim preset's own tree with the knob on and would otherwise recurse.
if(NOT _fs_preset_key STREQUAL "sim" OR KICKOS_MULTI_INSTANCE)
  return()
endif()
add_test(NAME sim_multi_instance
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_multi_instance.sh"
          "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
set_tests_properties(sim_multi_instance PROPERTIES TIMEOUT 300)
