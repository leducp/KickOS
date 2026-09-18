# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Use the two-core fixture to test scheduler lock assertions; single-core
# builds do not track lock depth.
kickos_add_unit_test(NAME held_assert SOURCES held_assert.cc
  LIBRARIES kickos_kseam_smp)
