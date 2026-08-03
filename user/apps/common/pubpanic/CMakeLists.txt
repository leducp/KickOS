# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Terminal reporting on a published console: one source, two images (kos_panic, and an
# illegal instruction through the arch fault reporter). Diagnostic app
# (KICKOS_ENABLE_SELFTEST only); sim-only because case 2 traps via __builtin_trap and
# the published posture is only automatable on the host.
if(NOT KICKOS_ARCH STREQUAL "sim")
  return()
endif()

foreach(_case 1 2)
  kickos_add_diagnostic_app(pubpanic${_case}
    SOURCES main.cc
    BOARD   ${KICKOS_BOARD})
  if(NOT TARGET pubpanic${_case})
    return() # diagnostics off -> not built; skip the test
  endif()
  target_compile_definitions(pubpanic${_case} PRIVATE KICKOS_PUBPANIC_CASE=${_case})
endforeach()

# The gate builds its OWN tree with kickos_services_sim (one service-list provider links
# per image), so it must register only in the ordinary tree; registering it in the tree
# the script itself configures would recurse.
if(KICKOS_BUILD_TESTS AND KICKOS_SERVICE_LIST STREQUAL "kickos_services_none")
  add_test(
    NAME    sim_published_panic
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_sim_pubpanic.sh"
            "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
  set_tests_properties(sim_published_panic PROPERTIES TIMEOUT 300)
endif()
