# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The fault reporter's line bound. Built in BOTH diagnostic postures: KICKOS_DIAG_TERSE picks
# a whole column of the catalogue, so one build reads only half of what the bound has to hold.
kickos_add_unit_test(NAME fault_line
  SOURCES faultline.cc "${PROJECT_SOURCE_DIR}/lib/libc/fmt.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/lib/include" "${PROJECT_SOURCE_DIR}/include")

kickos_add_unit_test(NAME fault_line_terse
  SOURCES faultline.cc "${PROJECT_SOURCE_DIR}/lib/libc/fmt.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/lib/include" "${PROJECT_SOURCE_DIR}/include"
  DEFINITIONS KICKOS_DIAG_TERSE=1
  TEST_PREFIX terse.)
