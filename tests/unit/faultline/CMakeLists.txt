# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The fault reporter's line bound.
kickos_add_unit_test(NAME fault_line
  SOURCES faultline.cc "${PROJECT_SOURCE_DIR}/lib/libc/fmt.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/lib/include" "${PROJECT_SOURCE_DIR}/include")
