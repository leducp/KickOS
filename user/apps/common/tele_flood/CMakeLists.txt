# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Telemetry ring wrap / full-drop gate (spike CI gate 2): overflow the ch1 ring,
# assert record-atomicity + drop accounting. Only meaningful with telemetry on.
kickos_add_application(tele_flood
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_test(
    NAME    telemetry_ring_wrap
    COMMAND "${PROJECT_SOURCE_DIR}/tests/telemetry/check_flood.py"
            "$<TARGET_FILE:tele_flood>"
            "${PROJECT_SOURCE_DIR}/tools/kicktrace.py")
  set_tests_properties(telemetry_ring_wrap PROPERTIES TIMEOUT 60)
endif()
