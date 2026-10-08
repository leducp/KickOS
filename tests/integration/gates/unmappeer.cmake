# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gate riding `unmappeer`: a page withdrawn while a peer core reads it.

if(NOT TARGET unmappeer)
  return()
endif()

kickos_add_qemu_test(TARGET unmappeer
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_unmappeer.sh"
  ARGS "THREAD FAULT" 139)
