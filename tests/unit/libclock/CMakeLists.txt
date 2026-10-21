# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# user/src/newlib_lock.cc over the real user/src/mutex.cc and the sim's compare-and-swap, which
# the arm wraps to count the takes the fast path completes alone, with a scripted kernel behind
# the two contended entries. newlib/ stands in for the toolchain's headers, which the host libc
# does not have.
kickos_add_unit_test(NAME libc_lock
  SOURCES libc_lock.cc "${PROJECT_SOURCE_DIR}/user/src/newlib_lock.cc"
          "${PROJECT_SOURCE_DIR}/user/src/mutex.cc"
          "${PROJECT_SOURCE_DIR}/user/src/arch/sim/user_cas.cc"
  INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/newlib" "${PROJECT_SOURCE_DIR}/user/include"
           "${PROJECT_SOURCE_DIR}/system/include" "${PROJECT_BINARY_DIR}/generated/include"
  KERNEL)
target_link_options(libc_lock PRIVATE -Wl,--wrap=kickos_user_cas)
