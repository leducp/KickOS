# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Where a READY thread is placed across cores and what a core whose level falls asks for, over
# the K-seam sources AT TWO KERNEL CORES. Those are already compiled by tests/unit/exitquiesce as
# kickos_kseam_smp, whose target must therefore exist before this directory is added.
kickos_add_unit_test(NAME placement SOURCES placement.cc
  LIBRARIES kickos_kseam_smp)
