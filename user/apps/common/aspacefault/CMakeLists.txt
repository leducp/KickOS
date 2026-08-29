# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The address-space fault gate. Own binary (it ends the process) and built only where a
# translating backend exists, since the scenario it asks for has no meaning on a board that
# describes regions. Diagnostic app: never in a production image.
if(NOT KICKOS_HAVE_ASPACE)
  return()
endif()

kickos_add_diagnostic_app(aspacefault
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})
if(NOT TARGET aspacefault)
  return() # diagnostics off -> app not built; skip its tests
endif()

# 132 is kfault_terminate's status and "ARMV8A EXCEPTION" the panic banner this arch prints;
# both are restated here rather than computed from the sources that produce them, so the gate
# asserts something about them.
#
# STILL THE PANIC BANNER WITH FAULT ISOLATION LIVE, and that is the scenario rather than a
# gap: the read this arm makes is the KERNEL's own, inside op_touch_unmapped, so it arrives at
# the current-EL vector and the kill rule declines it. Isolation contains a thread's fault, and
# a kernel dereference of an unmapped page is a kernel bug (docs/design-m6-mmu.md F5, T8).
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-arm64")
  kickos_add_qemu_test(NAME qemu_arm64_aspace_fault TARGET aspacefault BOARD qemu-arm64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_aspace_fault.sh"
    ARGS "ARMV8A EXCEPTION" 132)
endif()

# The same arm on RV64, through this architecture's own script: the banner is this arch's
# (`=== RISC-V S-TRAP`), the reported address is `stval=` rather than `FAR=`, and `scause`
# names no translation level.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-riscv64")
  kickos_add_qemu_test(NAME qemu_riscv64_aspace_fault TARGET aspacefault BOARD qemu-riscv64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_aspace_fault_rv64.sh"
    ARGS "RISC-V S-TRAP" 132)
endif()
