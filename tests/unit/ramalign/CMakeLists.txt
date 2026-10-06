# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# arch_ram_region_align where the thread pointer is SP masked down to the stride, which no host
# build is: the posture is set here, and the region seam is the fixture's.
kickos_add_unit_test(NAME ram_align
  SOURCES stride_align.cc
  POSTURE KICKOS_TLS=1 KICKOS_TLS_FROM_SP=1 KICKOS_TLS_STRIDE=8192 KICKOS_TLS_STRIDE_LOG2=13)
