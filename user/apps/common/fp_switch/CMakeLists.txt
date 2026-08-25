# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_application(fp_switch
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

# QEMU armv7m gate: boot the image, assert the checker reports s16-s31 preserved
# ("FP OK") and never "FP FAIL". Validates the PendSV FP save/restore on real armv7m
# before flashing. Runs on every MPS2 image with an FPU; the enclosing
# add_subdirectory gate decides that (user/apps/common/CMakeLists.txt).
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD IN_LIST KICKOS_QEMU_MPS2_BOARDS)
  kickos_add_qemu_test(NAME ${KICKOS_QEMU_MPS2_TAG}_fp_switch TARGET fp_switch BOARD ${KICKOS_BOARD}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_fp.sh")
endif()

# qemu-arm64: the same torture over ALL THIRTY-TWO vector registers. A64 auto-stacks no FP
# state at all, so an asynchronous entry owes the whole bank and not just the callee-saved
# half the M-profile arm above witnesses.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu-arm64")
  kickos_add_qemu_test(NAME qemu_arm64_fp_switch TARGET fp_switch BOARD qemu-arm64
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_qemu_fp.sh")
endif()
