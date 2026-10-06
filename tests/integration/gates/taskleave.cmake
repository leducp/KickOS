# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The address-space leave gate riding `taskleave`: a space installed on a peer core, which is
# the whole hazard.

if(NOT TARGET taskleave)
  return()
endif()

kickos_add_qemu_test(TARGET taskleave
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_taskleave.sh")
