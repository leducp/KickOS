# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# CI gate 1: golden-vector encode->decode round-trip. A host program drives the
# pure encoders (include/kickos/trace/record.h -- header-only, no KickOS libs);
# kicktrace.py decodes and must match exactly. Host-only (must run on the build
# host), so it is registered on the sim build only.
add_executable(kicktrace_gen_golden gen_golden.cc)
target_compile_options(kicktrace_gen_golden PRIVATE ${KICKOS_WARN_FLAGS})

add_test(
  NAME    telemetry_golden
  COMMAND "${CMAKE_COMMAND}" -E env
          "${PROJECT_SOURCE_DIR}/tests/telemetry/check_golden.py"
          "$<TARGET_FILE:kicktrace_gen_golden>"
          "${PROJECT_SOURCE_DIR}/tools/kicktrace.py")
set_tests_properties(telemetry_golden PROPERTIES TIMEOUT 30)
