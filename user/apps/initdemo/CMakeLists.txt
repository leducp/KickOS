# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# M4.3 per-thread stdout regression. newlib_stubs.cc (the _write under test) is
# compiled ONLY for the cross toolchains, NOT the sim, so the sim gives zero
# coverage. qemu (mps2-an386) is the automatable armv7m gate: it runs and exits, and
# arch_shutdown forwards the exit status over semihosting. Registered for the qemu
# board only (non-enforcement; all threads share one address space, so the app's
# globals are a valid cross-thread channel).
if(NOT KICKOS_BOARD STREQUAL "qemu")
  return()
endif()

kickos_add_application(initdemo
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

# The verdict rides the exit status (console is dark post-publish), so the harness
# reads QEMU's process exit code rather than grepping stdout.
if(KICKOS_BUILD_TESTS)
  add_test(
    NAME    qemu_initdemo
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_qemu_initdemo.sh"
            "$<TARGET_FILE:initdemo>")
  set_tests_properties(qemu_initdemo PROPERTIES TIMEOUT 40 SKIP_RETURN_CODE 77)
endif()
