# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Telemetry structural gate (spike CI gate 3), riding `tele_pingpong`: a bounded workload whose
# ch1 trace is decoded + structurally asserted. Only meaningful with telemetry enabled.

if(NOT TARGET tele_pingpong)
  return()
endif()

if(KICKOS_ARCH STREQUAL "sim")
  add_test(
    NAME    telemetry_structural
    COMMAND "${PROJECT_SOURCE_DIR}/tests/integration/telemetry/check_run.py"
            "$<TARGET_FILE:tele_pingpong>"
            "${PROJECT_SOURCE_DIR}/tools/kicktrace.py")
  set_tests_properties(telemetry_structural PROPERTIES TIMEOUT 40)
else()
  # CI gate 4: the same bounded workload booted under QEMU, the only automated
  # coverage of the PendSV-tail switch-hook ASM. Trace captured via semihosting.
  kickos_add_qemu_test(NAME telemetry_qemu_structural TARGET tele_pingpong
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/telemetry/check_qemu.py"
    BOOTS 0 WORK 40 ARGS "${PROJECT_SOURCE_DIR}/tools/kicktrace.py")
endif()
