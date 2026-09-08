# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A slay whose victim is RUNNING on a PEER core, over the K-seam sources AT TWO KERNEL CORES.
# Those are already compiled by tests/unit/exitquiesce as kickos_kseam_smp, whose target must
# therefore exist before this directory is added.
kickos_add_unit_test(NAME peer_slay SOURCES peer_slay.cc
  LIBRARIES kickos_kseam_smp)
