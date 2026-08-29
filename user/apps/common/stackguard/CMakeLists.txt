# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The stack guard-page gate. Own binary (it ends the process) and built only where a
# translating backend exists: on a region backend a thread stack is one descriptor over an
# arena block and there is no unmapped page below it to find. Diagnostic app: never in a
# production image.
if(NOT KICKOS_HAVE_ASPACE)
  return()
endif()

kickos_add_diagnostic_app(stackguard
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET stackguard)
  return() # diagnostics off -> app not built; skip its tests
endif()

# 139 is KOS_EXIT_FAULT and "THREAD FAULT" the thread-kill banner; both are restated here
# rather than computed from the sources that produce them, so the gate asserts something about
# them. The access is an EL0 one, so armv8a CONTAINS it (docs/design-m6-mmu.md F5, T8): the
# faulting thread's task dies and the marker is the kill banner, not the panic dump.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-arm64")
  kickos_add_qemu_test(NAME qemu_arm64_stack_guard TARGET stackguard BOARD qemu-arm64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_stack_guard.sh"
    ARGS "THREAD FAULT" 139)
endif()

# The same arm on RV64, through this architecture's own script: the write bit lives in the
# CAUSE here rather than beside a level field, so there is no constant to substitute.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv64")
  kickos_add_qemu_test(NAME qemu_riscv64_stack_guard TARGET stackguard BOARD qemu-riscv64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_stack_guard_rv64.sh"
    ARGS "THREAD FAULT" 139)
endif()
