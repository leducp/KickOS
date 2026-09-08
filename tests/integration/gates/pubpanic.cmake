# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The terminal-reporting arm riding the two `pubpanic` images: kos_panic, and an illegal
# instruction through the arch fault reporter.

if(NOT TARGET pubpanic1)
  return()
endif()

# The gate builds its OWN tree with kickos_services_sim (one service-list provider links
# per image), so it must register only in the ordinary tree; registering it in the tree
# the script itself configures would recurse.
if(KICKOS_SERVICE_LIST STREQUAL "kickos_services_none")
  add_test(
    NAME    sim_published_panic
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_sim_pubpanic.sh"
            "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}" ${KICKOS_FAULT_OUTCOME})
  set_tests_properties(sim_published_panic PROPERTIES TIMEOUT 300)
endif()
