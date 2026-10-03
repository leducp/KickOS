# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# tests/drivers ships with no package that leaves KICKOS_TEST_DRIVERS off
# (tests/integration/check_test_drivers_absent.sh).

if(KICKOS_TEST_DRIVERS)
  return()
endif()
add_test(NAME test_drivers_absent
  COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/check_test_drivers_absent.sh"
          "${PROJECT_BINARY_DIR}" "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}")
kickos_host_gate(test_drivers_absent TIMEOUT 300)
