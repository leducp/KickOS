# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The timed semaphore wait against a post, a cancel and its own deadline, over the real
# time.cc. Host-only of necessity: the post and the expiry must reach one park in a chosen
# order, which no image can arrange. The arms SEQUENCE the two, they do not race them: that
# they never interleave rests on both taking the kernel's exclusion, which the debug assertions
# at the queue's pop and at the expiry's unlink check at two kernel cores.
#
# Needs kickos_kseam_time (tests/unit/parkdeath) to exist before this directory is added.
kickos_add_unit_test(NAME sem_timed SOURCES sem_timed.cc
  LIBRARIES kickos_kseam_time)

kickos_add_kseam(kickos_kseam_time_smp EXTRA "${PROJECT_SOURCE_DIR}/kernel/time/time.cc"
  POSTURE KICKOS_NUM_CORES=2 KICKOS_KERNEL_CORES=2 KICKOS_MULTICORE_MODEL_SHARED=1)
target_compile_definitions(kickos_kseam_time_smp PRIVATE KFIXTURE_REAL_TIME=1)
kickos_add_unit_test(NAME sem_timed_smp SOURCES sem_timed.cc
  LIBRARIES kickos_kseam_time_smp
  TEST_PREFIX smp.)
