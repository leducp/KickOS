# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The cross-core slay gate riding `slaypeer`: a victim RUNNING on a core other than the
# slayer's, which is the whole hazard.

if(NOT TARGET slaypeer)
  return()
endif()

if(KICKOS_BOARD STREQUAL "qemu-arm64" OR KICKOS_BOARD STREQUAL "qemu-riscv64")
  kickos_add_qemu_test(TARGET slaypeer
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_slaypeer.sh"
    TIMEOUT 120)
endif()
