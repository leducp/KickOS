# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# user/src/cond.cc over the real user/src/mutex.cc, the sim's compare-and-swap, which the arm
# wraps to land a peer's signal at the waiter's own steps, and the real sync.cc, park.cc and
# time.cc behind the user library's kernel entries, at one and two kernel cores.
#
# Needs kickos_kseam_time (tests/unit/parkdeath) and kickos_kseam_time_smp (tests/unit/semtimed)
# to exist before this directory is added.
set(_cond_sources cond_wait.cc "${PROJECT_SOURCE_DIR}/user/src/cond.cc"
    "${PROJECT_SOURCE_DIR}/user/src/mutex.cc" "${PROJECT_SOURCE_DIR}/user/src/sem.cc"
    "${PROJECT_SOURCE_DIR}/user/src/arch/sim/user_cas.cc")
set(_cond_includes "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include"
    "${PROJECT_BINARY_DIR}/generated/include")
kickos_add_unit_test(NAME user_cond SOURCES ${_cond_sources} INCLUDES ${_cond_includes}
  LIBRARIES kickos_kseam_time)
kickos_add_unit_test(NAME user_cond_smp SOURCES ${_cond_sources} INCLUDES ${_cond_includes}
  LIBRARIES kickos_kseam_time_smp
  TEST_PREFIX smp.)
foreach(_cond IN ITEMS user_cond user_cond_smp)
  target_link_options(${_cond} PRIVATE -Wl,--wrap=kickos_user_cas)
endforeach()
