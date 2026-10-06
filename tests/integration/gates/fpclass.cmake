# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The double classification and printf gate riding `fpclass`. rxv3 has no QEMU machine, so
# kickos_add_qemu_test registers nothing there and the script judges the board's capture; the
# compiler half is arch/CMakeLists.txt's rx_dfpu_compare.

if(NOT TARGET fpclass)
  return()
endif()
kickos_app_judge(fpclass tests/integration/check_fpclass.sh)

kickos_add_qemu_test(TARGET fpclass
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_fpclass.sh")
