# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A round-robin slice that ran out while its thread was preempted, taken on the host fixture.
kickos_add_unit_test(NAME rr_slice SOURCES slice_spent.cc
  LIBRARIES kickos_kseam)
