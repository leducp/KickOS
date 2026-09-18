# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Check task-pool exhaustion while thread and domain slots remain available.
kickos_add_unit_test(NAME task_pool SOURCES pool_exhaustion.cc
  LIBRARIES kickos_kseam)
