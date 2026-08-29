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

# 139 is KOS_EXIT_FAULT and "THREAD FAULT" the thread-kill banner; both are restated here
# rather than computed from the sources that produce them, so the gate asserts something about
# them. The access is an EL0 one, so armv8a CONTAINS it (docs/design-m6-mmu.md F5, T8): the
# faulting thread's task dies and the marker is the kill banner, not the panic dump.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-arm64")
  kickos_add_qemu_test(NAME qemu_arm64_kernel_half TARGET kernelhalf BOARD qemu-arm64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_kernel_half.sh"
    ARGS "THREAD FAULT" 139)
endif()

# The same arm on RV64, through a script of this architecture's own and not the one above with
# a substituted constant: `scause` carries no fault-status field, so a permission fault and a
# translation fault are the same number and the assertion that separates them cannot be ported
# (tests/integration/check_kernel_half_rv64.sh says what stands in for it).
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv64")
  kickos_add_qemu_test(NAME qemu_riscv64_kernel_half TARGET kernelhalf BOARD qemu-riscv64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_kernel_half_rv64.sh"
    ARGS "THREAD FAULT" 139)
endif()
