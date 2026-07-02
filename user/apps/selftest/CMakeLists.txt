# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The M0 verification app + the CI gates.
kickos_add_application(selftest
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  # Full M0 verification: every scheduler/sync/time/MPU bullet, in order.
  # Needs KICKOS_ENABLE_SELFTEST (the checker asserts the guard-probe marker);
  # the sim preset enables it. Skipped otherwise so the case can actually pass.
  if(KICKOS_ENABLE_SELFTEST)
    add_test(
      NAME    selftest
      COMMAND "${CMAKE_COMMAND}" -E env
              "${PROJECT_SOURCE_DIR}/tests/check_selftest.py"
              "$<TARGET_FILE:selftest>")
    set_tests_properties(selftest PROPERTIES TIMEOUT 30)
  endif()

  # Dependency-inversion acceptance: an out-of-tree app builds against the
  # exported KickOS package (find_package + kickos_add_application) and runs.
  add_test(
    NAME    oot_export
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_oot_export.sh"
            "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}"
            "${CMAKE_COMMAND}" "${CMAKE_GENERATOR}")
  set_tests_properties(oot_export PROPERTIES TIMEOUT 120)
endif()
