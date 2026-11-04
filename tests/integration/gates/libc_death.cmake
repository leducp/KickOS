# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The gate riding `libc_death` and `libc_death_env`: a dead holder's libc lock ends its next taker,
# stdout's in the one and the environment's in the other.

if(NOT TARGET libc_death)
  return()
endif()
set(_libc_death_outcome ${KICKOS_FAULT_OUTCOME})
foreach(_libc_death_app libc_death libc_death_env)
  kickos_app_judge(${_libc_death_app} tests/integration/check_libc_death.sh
    ARGS ${_libc_death_outcome})
  kickos_add_qemu_test(TARGET ${_libc_death_app}
    SCRIPT "${PROJECT_SOURCE_DIR}/tests/integration/check_libc_death.sh"
    ARGS ${_libc_death_outcome})
endforeach()
