# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Task-scoped death and the group kill. Host-only for the fixture's own reason: each arm
# seats a peer parked under one chosen wait kind (the fixture's park_* helpers, plus a
# hand-seated WAIT_TASK_EMPTY), which a running image reaches only by luck, and because
# the claim is about ORDER, which has no on-target observable.
kickos_add_unit_test(NAME task_death SOURCES group_kill.cc creator_hold.cc
  LIBRARIES kickos_kseam)
