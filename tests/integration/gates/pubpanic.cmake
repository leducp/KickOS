# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The terminal-reporting arm riding the two `pubpanic` images, each on the packaged simcon its
# composition names as stdout: kos_panic, and an illegal instruction through the arch fault
# reporter.

if(NOT TARGET pubpanic1)
  return()
endif()

add_test(NAME sim_published_panic
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_pubpanic.sh"
          "$<TARGET_FILE:pubpanic1>" "$<TARGET_FILE:pubpanic2>" ${KICKOS_FAULT_OUTCOME})
set_tests_properties(sim_published_panic PROPERTIES TIMEOUT 120)
