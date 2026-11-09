# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Task-scoped death and the group kill. Host-only for the fixture's own reason: each arm
# seats a peer parked under one chosen wait kind (the fixture's park_* helpers, plus a
# hand-seated WAIT_TASK_EMPTY), which a running image reaches only by luck, and because
# the claim is about ORDER, which has no on-target observable. sweep_wait.cc runs one member's
# whole exit inside another's sweep gap, an interleaving one core reaches only through a wake.
kickos_add_unit_test(NAME task_death SOURCES group_kill.cc creator_hold.cc sweep_wait.cc
  LIBRARIES kickos_kseam)

# A lend's drain at its pop bound, which the posture lowers to one pop so a two-hop chain reaches
# it: every death waiter stays linked once and nothing past the bound is lent to.
kickos_add_kseam(kickos_kseam_pidrain POSTURE KICKOS_PI_DRAIN_POPS=1)
kickos_add_unit_test(NAME pi_drain_cap SOURCES drain_cap.cc
  LIBRARIES kickos_kseam_pidrain)

# Death waiters timing out in one timer interrupt, over the real time.cc: the members keep no
# lend of a waiter gone, and the current the lend held up gives way to the highest waiter.
kickos_add_kseam(kickos_kseam_death_time EXTRA "${PROJECT_SOURCE_DIR}/kernel/time/time.cc"
  "${PROJECT_SOURCE_DIR}/kernel/time/realtime.cc")
target_compile_definitions(kickos_kseam_death_time PRIVATE KFIXTURE_REAL_TIME=1)
kickos_add_unit_test(NAME death_timeouts SOURCES death_timeouts.cc
  LIBRARIES kickos_kseam_death_time)

# The leave batch at a two-instance posture, which the sim build's own definitions do not give.
kickos_add_kseam(kickos_kseam_instances POSTURE KICKOS_MULTI_INSTANCE=1 KICKOS_MAX_INSTANCES=2)
kickos_add_unit_test(NAME death_instances SOURCES death_instances.cc
  LIBRARIES kickos_kseam_instances)
