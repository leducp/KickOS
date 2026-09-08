# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The reschedule a cross-core raise carries, against a consumer that absorbs the raise.
# kernel/sync/klock.cc is compiled here as the real kernel translation unit, and
# resched_seam.cc answers its whole undefined set.
#
# AT TWO KERNEL CORES, which is the point and is why this gate compiles its own copy rather
# than joining the K-seam fixture: that library takes the preset's core count, and the sim
# preset drives one core, where the mechanism under test compiles to nothing.
kickos_add_unit_test(NAME resched_owed
  SOURCES owed.cc resched_seam.cc
          "${PROJECT_SOURCE_DIR}/kernel/sync/klock.cc"
  POSTURE KICKOS_NUM_CORES=2 KICKOS_KERNEL_CORES=2 KICKOS_MULTICORE_MODEL_SHARED=1)
