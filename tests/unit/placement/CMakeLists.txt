# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Where a READY thread is placed across cores, what a core whose level falls asks for, and the
# cross-core ask a re-placement owes (migrate_ask.cc), over the K-seam sources AT TWO KERNEL
# CORES. Those are already compiled by tests/unit/exitquiesce as kickos_kseam_smp, whose target
# must therefore exist before this directory is added.
kickos_add_unit_test(NAME placement SOURCES placement.cc migrate_ask.cc
  LIBRARIES kickos_kseam_smp)
