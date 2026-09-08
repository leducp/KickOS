# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# What a blocking primitive returns, and the fixture mechanism that makes the answer real.
# Host-only of necessity: on target a park's return value is indistinguishable from a wrong
# one, because the waker that wrote it is another thread and no image can withhold it.
kickos_add_unit_test(NAME park_result SOURCES park_result.cc
  LIBRARIES kickos_kseam)
