# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The per-thread stdout regression riding `initdemo`. newlib_stubs.cc (the _write under test) is
# compiled ONLY for the cross toolchains, NOT the sim, so the sim gives zero coverage. The QEMU
# mps2 images are the automatable armv7m gates: they run and exit, and arch_shutdown forwards
# the exit status over semihosting. Those boards are also the only ones where all threads share
# one address space, which is what makes the app's globals a valid cross-thread channel.
#
# The verdict rides the exit status (console is dark post-publish), so the harness reads QEMU's
# process exit code rather than grepping stdout.

if(NOT TARGET initdemo)
  return()
endif()

kickos_add_qemu_test(TARGET initdemo
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_initdemo.sh" TIMEOUT 40)
