# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gate riding `aspacefault`: a kernel read of an unmapped address.

if(NOT TARGET aspacefault)
  return()
endif()

# 132 is kfault_terminate's status, and the marker each arch's banner for a fault the kernel
# took itself. The gate asserts them as literals; computing them from the sources that produce
# them would assert nothing. The faulting read is the KERNEL's own, inside op_touch_unmapped, so
# it arrives at the kernel's own trap vector and the record is that banner.
# x86_64's ring-0 exception report halts rather than exiting, so its script polls for the report.
if(KICKOS_ARCH STREQUAL "x86_64")
  kickos_add_qemu_test(NAME ${_tag}_aspace_fault TARGET aspacefault
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_aspace_fault_x86_64.sh"
    ARGS "X86_64 EXCEPTION")
endif()

if(KICKOS_ARCH STREQUAL "armv8a")
  kickos_add_qemu_test(NAME ${_tag}_aspace_fault TARGET aspacefault
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_aspace_fault.sh"
    ARGS "ARMV8A EXCEPTION" 132 ${KICKOS_ARCH})
endif()

if(KICKOS_ARCH STREQUAL "rv64imac")
  kickos_add_qemu_test(NAME ${_tag}_aspace_fault TARGET aspacefault
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_aspace_fault.sh"
    ARGS "RISC-V S-TRAP" 132 ${KICKOS_ARCH})
endif()
