# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Telemetry ring wrap / full-drop gate (spike CI gate 2), riding `tele_flood`: overflow the ch1
# ring, assert record-atomicity + drop accounting. Only meaningful with telemetry on.

if(NOT TARGET tele_flood)
  return()
endif()

if(KICKOS_ARCH STREQUAL "sim")
  add_test(
    NAME    telemetry_ring_wrap
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/telemetry/check_flood.py"
            "$<TARGET_FILE:tele_flood>"
            "${PROJECT_SOURCE_DIR}/tools/kicktrace.py")
  # Above check_flood.py's own 60 s wait for the app.
  set_tests_properties(telemetry_ring_wrap PROPERTIES TIMEOUT 70)
endif()
