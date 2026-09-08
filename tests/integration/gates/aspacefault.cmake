# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gate riding `aspacefault`: a kernel read of an unmapped address, armv8a only.

if(NOT TARGET aspacefault)
  return()
endif()

# 132 is kfault_terminate's status and "ARMV8A EXCEPTION" this arch's panic banner. The gate
# asserts them as literals; computing them from the sources that produce them would assert
# nothing. The faulting read is the KERNEL's own, inside op_touch_unmapped, so it arrives at the
# current-EL vector and the record is the panic banner.
if(KICKOS_ARCH STREQUAL "armv8a")
  kickos_add_qemu_test(NAME ${_tag}_aspace_fault TARGET aspacefault
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_aspace_fault.sh"
    ARGS "ARMV8A EXCEPTION" 132)
endif()
