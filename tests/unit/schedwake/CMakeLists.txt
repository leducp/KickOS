# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The K-seam fixture's first customer: sched::wake's behaviour while the current thread is
# dying, and the order of exit_current's own waiter loop. Host-only of necessity twice over:
# a switch DECISION has no on-target observable except run order, and the past-exit phase
# needs a state a real run reaches only for the few microseconds it takes to end.
kickos_add_kseam_gate(NAME sched_wake SOURCES wake_dying.cc)
