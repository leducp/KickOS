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

# CI gate 3: trace-metadata de-drift. A host program prints the arch/syscall numbers
# the C++ side actually spells (record.h ArchId + abi.h kos_syscall_nr); check_idmap.py
# takes abi.h as the syscall authority and asserts it, that list and kicktrace.py's
# SYSCALL_NAME all agree.
# Needs user/include (abi.h) + system/include (errno.h); include/ is already global.
add_executable(kicktrace_gen_idmap gen_idmap.cc)
target_include_directories(kicktrace_gen_idmap PRIVATE
  "${PROJECT_SOURCE_DIR}/user/include"
  "${PROJECT_SOURCE_DIR}/system/include")
target_compile_options(kicktrace_gen_idmap PRIVATE ${KICKOS_WARN_FLAGS})

add_test(
  NAME    telemetry_idmap
  COMMAND "${CMAKE_COMMAND}" -E env
          "${PROJECT_SOURCE_DIR}/tests/telemetry/check_idmap.py"
          "$<TARGET_FILE:kicktrace_gen_idmap>"
          "${PROJECT_SOURCE_DIR}/tools/kicktrace.py"
          "${PROJECT_SOURCE_DIR}/user/include/kickos/sys/abi.h")
set_tests_properties(telemetry_idmap PROPERTIES TIMEOUT 30)
