# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The NVIC line count of the MPS2 machine against the chip file's.
if(KICKOS_QEMU_MPS2 AND TARGET hello)
  kickos_add_qemu_test(TARGET hello NAME ${_tag}_nvic
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_nvic.sh"
    ARGS "${PROJECT_BINARY_DIR}/generated/include/kickos/chip_limits.h")
endif()
