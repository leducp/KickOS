# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gate riding `libc_death`: a dead holder's libc lock ends its next taker.

if(NOT TARGET libc_death)
  return()
endif()
# On a chip that holds one task the take ends the whole image, with no fault report.
set(_libc_death_outcome ${KICKOS_FAULT_OUTCOME})
if(KICKOS_CHIP_ONE_TASK)
  set(_libc_death_outcome image)
endif()
kickos_app_judge(libc_death tests/integration/check_libc_death.sh ARGS ${_libc_death_outcome})
kickos_add_qemu_test(TARGET libc_death
  SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_libc_death.sh"
  ARGS ${_libc_death_outcome})
