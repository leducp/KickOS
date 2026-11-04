# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# user/src/newlib_sbrk.cc over a window the arm lays out, its allocator calls answered as newlib's
# full malloc answers its first request. newlib/ stands in for the toolchain's newlib.h, which the
# host libc does not have; the renames keep the host libc's own names out of that object alone.
kickos_add_unit_test(NAME heap_break
  SOURCES heap_break.cc sbrk_shim.cc
  INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/newlib" "${PROJECT_SOURCE_DIR}/user/include"
           "${PROJECT_SOURCE_DIR}/system/include" "${PROJECT_SOURCE_DIR}/lib/include"
           "${PROJECT_BINARY_DIR}/generated/include"
  KERNEL)
set_source_files_properties(sbrk_shim.cc PROPERTIES
  COMPILE_DEFINITIONS "sbrk=kos_ut_sbrk;malloc=kos_ut_malloc;free=kos_ut_free")
