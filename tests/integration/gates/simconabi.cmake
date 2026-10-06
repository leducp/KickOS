# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The console endpoint's framed arm, riding `simconabi` against the packaged simcon its
# composition names as stdout.

if(NOT TARGET simconabi)
  return()
endif()

add_test(NAME sim_console_abi
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_conabi.sh" "$<TARGET_FILE:simconabi>")
set_tests_properties(sim_console_abi PROPERTIES TIMEOUT 60)
