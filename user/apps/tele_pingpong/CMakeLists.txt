# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Telemetry structural gate (spike CI gate 3): a bounded workload whose ch1 trace
# is decoded + structurally asserted. Only meaningful with telemetry enabled.
kickos_add_application(tele_pingpong
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

if(KICKOS_BUILD_TESTS AND KICKOS_ARCH STREQUAL "sim")
  add_test(
    NAME    telemetry_structural
    COMMAND "${PROJECT_SOURCE_DIR}/tests/telemetry/check_run.py"
            "$<TARGET_FILE:tele_pingpong>"
            "${PROJECT_SOURCE_DIR}/tools/kicktrace.py")
  set_tests_properties(telemetry_structural PROPERTIES TIMEOUT 40)
endif()

# CI gate 4: the same bounded workload booted under QEMU -- the only automated
# coverage of the PendSV-tail switch-hook ASM. Trace captured via semihosting.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu")
  add_test(
    NAME    telemetry_qemu_structural
    COMMAND "${PROJECT_SOURCE_DIR}/tests/telemetry/check_qemu.py"
            "$<TARGET_FILE:tele_pingpong>"
            "${PROJECT_SOURCE_DIR}/tools/kicktrace.py")
  set_tests_properties(telemetry_qemu_structural PROPERTIES TIMEOUT 40
                       SKIP_RETURN_CODE 77)
endif()
