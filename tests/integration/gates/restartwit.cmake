# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A restart of image 1 starts from the image's data, riding `restartwit`.

if(NOT TARGET restartwit)
  return()
endif()
kickos_app_judge(restartwit tests/integration/check_restartwit.sh)
kickos_add_qemu_test(TARGET restartwit
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_restartwit.sh")
