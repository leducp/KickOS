# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Check task-pool exhaustion while thread and domain slots remain available.
kickos_add_unit_test(NAME task_pool SOURCES pool_exhaustion.cc
  LIBRARIES kickos_kseam)

# The thread pool's claim order, reclaimable list and children, and a task's holdings counted
# from its own members, over the budget posture taskbudget builds.
kickos_add_unit_test(NAME task_pool_lists SOURCES pool_lists.cc
  LIBRARIES kickos_kseam_budget)

# Every pooled type's default image is zero, which the pool layout fingerprint relies on.
kickos_add_unit_test(NAME pool_layout SOURCES pool_layout.cc
  LIBRARIES kickos_kseam)

# task_admit over THREADS through a member's sweep, and RAM summed over a record's incarnations.
kickos_add_kseam(kickos_kseam_admit
  POSTURE KICKOS_SEAM_BUDGET_THREADS=4 KICKOS_SEAM_BUDGET_RAM_BLOCKS=3
          KICKOS_SEAM_BUDGET_RAM_BYTES=4096)
kickos_add_unit_test(NAME task_budget_admit SOURCES budget_admit.cc
  LIBRARIES kickos_kseam_admit)
# The same arms where no region unit is programmed.
kickos_add_kseam(kickos_kseam_admit_flat
  POSTURE KICKOS_HAVE_MPU=0 KICKOS_MEMORY_ENFORCED=0
          KICKOS_SEAM_BUDGET_THREADS=4 KICKOS_SEAM_BUDGET_RAM_BLOCKS=3
          KICKOS_SEAM_BUDGET_RAM_BYTES=4096)
kickos_add_unit_test(NAME task_budget_admit_flat TEST_PREFIX flat_ SOURCES budget_admit.cc
  LIBRARIES kickos_kseam_admit_flat)
