# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Task-scoped death and the group kill. Host-only for the fixture's own reason -- these arms
# seat a thread parked in each of the six park kinds at once, which a running image reaches
# only by luck -- and because the claim is about ORDER, which has no on-target observable.
kickos_add_kseam_gate(NAME task_death SOURCES group_kill.cc creator_hold.cc)
