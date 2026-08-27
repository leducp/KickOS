# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The kernel-half revocation gate. Own binary (it ends the process) and built only where a
# translating backend exists: on a region backend the kernel's state is refused by a
# descriptor set and the arm that witnesses that is the MPU guard. Diagnostic app: never in
# a production image.
if(NOT KICKOS_HAVE_ASPACE)
  return()
endif()

kickos_add_diagnostic_app(kernelhalf
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET kernelhalf)
  return() # diagnostics off -> app not built; skip its tests
endif()

# 132 is kfault_terminate's status and "ARMV8A EXCEPTION" the panic banner this arch prints;
# both are restated here rather than computed from the sources that produce them, so the gate
# asserts something about them. This arch isolates no fault yet, so the thread does not
# survive and the marker is the panic banner rather than a THREAD FAULT line.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-arm64")
  kickos_add_qemu_test(NAME qemu_arm64_kernel_half TARGET kernelhalf BOARD qemu-arm64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_kernel_half.sh"
    ARGS "ARMV8A EXCEPTION" 132)
endif()
