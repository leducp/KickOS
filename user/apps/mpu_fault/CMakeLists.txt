# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The memory-domain isolation gate, in its own binary (it ends the process on
# the deliberate cross-domain fault).
kickos_add_application(mpu_fault
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_test(NAME mpu_fault COMMAND "$<TARGET_FILE:mpu_fault>")
  set_tests_properties(mpu_fault PROPERTIES
    TIMEOUT 15
    PASS_REGULAR_EXPRESSION "MPU FAULT: task 'domainA'"
    FAIL_REGULAR_EXPRESSION "did not fault")
endif()
