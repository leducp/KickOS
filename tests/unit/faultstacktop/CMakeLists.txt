# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_fault_stack_top, where every backend's redirect seats the fault stub.
kickos_add_unit_test(NAME fault_stack_top SOURCES stack_top.cc
  LIBRARIES kickos_kseam)
