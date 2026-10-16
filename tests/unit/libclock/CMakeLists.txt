# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The kernel's libc locks over the K-seam fixture, at the preset's posture.
kickos_add_unit_test(NAME libc_lock_space SOURCES space.cc LIBRARIES kickos_kseam)

# user/src/newlib_lock.cc against a scripted kernel mutex. newlib/ stands in for the toolchain's
# headers, which the host libc does not have.
kickos_add_unit_test(NAME libc_lock
  SOURCES libc_lock.cc "${PROJECT_SOURCE_DIR}/user/src/newlib_lock.cc"
  INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/newlib" "${PROJECT_SOURCE_DIR}/user/include"
           "${PROJECT_SOURCE_DIR}/system/include" "${PROJECT_BINARY_DIR}/generated/include")
