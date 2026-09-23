# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A line's claim core against the bind and the wait's admission, over the K-seam sources AT TWO
# KERNEL CORES. Those are compiled by tests/unit/exitquiesce as kickos_kseam_smp, whose target
# must therefore exist before this directory is added.
kickos_add_unit_test(NAME irq_line_core SOURCES line_core.cc
  LIBRARIES kickos_kseam_smp)
