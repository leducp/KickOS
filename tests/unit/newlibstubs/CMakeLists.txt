# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# user/src/newlib_stubs.cc, compiled under prefixed names: the sim build links the host libc in
# its place, so only a host program can call the bodies the targets link.
kickos_add_unit_test(NAME newlib_stubs
  SOURCES newlib_stubs_shim.cc newlib_stubs_test.cc
  INCLUDES "${PROJECT_SOURCE_DIR}/user/include" "${PROJECT_SOURCE_DIR}/system/include"
           "${PROJECT_SOURCE_DIR}/lib/include" "${PROJECT_BINARY_DIR}/generated/include"
  KERNEL)
