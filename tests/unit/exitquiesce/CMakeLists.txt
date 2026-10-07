# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The exited-slot reclaim window. The K-seam sources again, AT TWO KERNEL CORES, which is why
# this gate builds its own posture library rather than joining kickos_kseam: that library takes
# the preset's core count, and the sim preset drives one core, where every body of the lock
# compiles to nothing and the span this gate asserts is not expressible.
#
# SHARED: every gate linking kickos_kseam_smp is added after this directory.
kickos_add_kseam(kickos_kseam_smp
  POSTURE KICKOS_NUM_CORES=2 KICKOS_KERNEL_CORES=2 KICKOS_MULTICORE_MODEL_SHARED=1)

# held_assert.cc: the scheduler's lock-held assertions, which one core does not track.
kickos_add_unit_test(NAME exit_quiesce SOURCES exit_span.cc held_assert.cc
  LIBRARIES kickos_kseam_smp)
