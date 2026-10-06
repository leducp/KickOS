# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The AMP partition's user share on a region board, over the REAL kernel/grant/grant.cc and
# kernel/mem/ramown.cc at an own-image node's posture with a share stated. No region board with
# a share has an emulator, so this is where the seat and the confinement exception run.
kickos_add_unit_test(NAME amp_share_admission
  SOURCES share_admission.cc
          "${PROJECT_SOURCE_DIR}/kernel/grant/grant.cc"
          "${PROJECT_SOURCE_DIR}/kernel/mem/ramown.cc"
  POSTURE KICKOS_AMP_OWN_IMAGE=1
          KICKOS_AMP_USER_SHARE_ADDR=0x2007C000
          KICKOS_AMP_USER_SHARE_SIZE=0x4000)
