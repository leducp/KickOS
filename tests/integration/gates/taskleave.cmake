# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The address-space leave gate riding `taskleave`: a space installed on a peer core, which is
# the whole hazard.

if(NOT TARGET taskleave)
  return()
endif()

if(KICKOS_BOARD STREQUAL "qemu-arm64" OR KICKOS_BOARD STREQUAL "qemu-riscv64")
  kickos_add_qemu_test(TARGET taskleave
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_taskleave.sh"
    TIMEOUT 120)
endif()
