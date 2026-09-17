# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The exclusion a HELD scheduler body asserts of its caller. Over kickos_kseam_smp, which
# tests/unit/exitquiesce defines and which carries kernel/sync/klock.cc at two kernel cores:
# the depth the assertion reads exists only there.
kickos_add_unit_test(NAME held_assert SOURCES held_assert.cc
  LIBRARIES kickos_kseam_smp)
