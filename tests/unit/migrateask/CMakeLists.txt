# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The cross-core ask a re-placement owes, over the K-seam sources AT TWO KERNEL CORES. Those
# are already compiled by tests/unit/exitquiesce as kickos_kseam_smp, whose target must
# therefore exist before this directory is added.
kickos_add_unit_test(NAME migrate_ask SOURCES migrate_ask.cc
  LIBRARIES kickos_kseam_smp)
