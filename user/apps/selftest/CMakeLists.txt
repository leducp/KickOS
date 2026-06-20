# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The M0 verification app + the CI gates. Links the freestanding TAP harness
# (tests/tap) directly; the app self-asserts and emits TAP over the console.
kickos_add_application(selftest
  SOURCES main.cc "${PROJECT_SOURCE_DIR}/tests/tap/tap.cc"
  BOARD   ${KICKOS_BOARD})
target_include_directories(selftest PRIVATE "${PROJECT_SOURCE_DIR}/tests/tap")

if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  # Full M0 verification: every scheduler/sync/time/MPU bullet as a TAP test.
  # The guard-probe test needs KICKOS_ENABLE_SELFTEST (the sim preset enables it);
  # the case is registered only then, so the plan count still matches.
  # The app's `int main` returns the failed-test count -> process exit status,
  # so CTest gates on the exit code (0 == all passed). FAIL regex is a backstop.
  add_test(NAME selftest COMMAND "$<TARGET_FILE:selftest>")
  set_tests_properties(selftest PROPERTIES
    TIMEOUT 30
    FAIL_REGULAR_EXPRESSION "not ok")

  # Dependency-inversion acceptance: an out-of-tree app builds against the
  # exported KickOS package (find_package + kickos_add_application) and runs.
  add_test(
    NAME    oot_export
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_oot_export.sh"
            "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}"
            "${CMAKE_COMMAND}" "${CMAKE_GENERATOR}")
  set_tests_properties(oot_export PROPERTIES TIMEOUT 120)
endif()
