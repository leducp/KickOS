# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# M4.3 per-thread stdout regression. newlib_stubs.cc (the _write under test) is
# compiled ONLY for the cross toolchains, NOT the sim, so the sim gives zero
# coverage. The QEMU mps2 images are the automatable armv7m gates: they run and exit,
# and arch_shutdown forwards the exit status over semihosting. Registered for those
# boards only (all threads share one address space there, so the app's globals are a
# valid cross-thread channel).
if(NOT KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  return()
endif()

kickos_add_application(initdemo
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

# The verdict rides the exit status (console is dark post-publish), so the harness
# reads QEMU's process exit code rather than grepping stdout.
if(KICKOS_BUILD_TESTS)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_initdemo TARGET initdemo BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/check_qemu_initdemo.sh" TIMEOUT 40)
endif()
