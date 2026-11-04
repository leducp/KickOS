# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# std::atomic read-modify-writes in user code riding `uatomic`, on every emulated board.

if(NOT TARGET uatomic)
  return()
endif()
kickos_app_judge(uatomic tests/integration/check_uatomic.sh)
kickos_add_qemu_test(TARGET uatomic SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_uatomic.sh")
