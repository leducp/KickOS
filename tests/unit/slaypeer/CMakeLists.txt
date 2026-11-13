# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A slay whose victim is RUNNING on a PEER core, over the K-seam sources AT TWO KERNEL CORES.
# Those are already compiled by tests/unit/exitquiesce as kickos_kseam_smp, whose target must
# therefore exist before this directory is added.
kickos_add_unit_test(NAME peer_slay SOURCES peer_slay.cc
  LIBRARIES kickos_kseam_smp)

# The group walk's order against its first switch, at two kernel cores: a member running on the
# peer is asked off before a running walker can be switched out.
kickos_add_unit_test(NAME task_slay_walk_smp
  SOURCES "${PROJECT_SOURCE_DIR}/tests/unit/taskdeath/slay_walk.cc"
  LIBRARIES kickos_kseam_smp)
