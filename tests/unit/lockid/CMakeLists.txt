# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The lock-id codec over the real ThreadPool: the handle libkickos_user derives from a seated id,
# and the generation the id carries through a reclaim and its revert.
kickos_add_unit_test(NAME lock_id SOURCES lock_id.cc
  LIBRARIES kickos_kseam)
