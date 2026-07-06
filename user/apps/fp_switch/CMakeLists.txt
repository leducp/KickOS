# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

kickos_add_application(fp_switch
  SOURCES main.cc
  BOARD   ${KICKOS_BOARD})

# QEMU armv7m (mps2-an386 = Cortex-M4F) gate: boot the image, assert the checker
# reports s16-s31 preserved ("FP OK") and never "FP FAIL". Validates the PendSV
# FP save/restore on real armv7m before flashing.
if(KICKOS_BUILD_TESTS AND KICKOS_BOARD STREQUAL "qemu")
  add_test(
    NAME    qemu_fp_switch
    COMMAND "${PROJECT_SOURCE_DIR}/tests/check_qemu_fp.sh" "$<TARGET_FILE:fp_switch>")
  set_tests_properties(qemu_fp_switch PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
endif()
