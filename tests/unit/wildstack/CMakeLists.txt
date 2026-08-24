# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The containment seam's refusals. Host-only of NECESSITY and not convenience: the rule
# cannot be reached from any image. No thread can be spawned privileged (the selftest arm
# privileged_spawn_refused asserts it), root is unprivileged from its first instruction, and
# idle is the only privileged thread but its body is arch_idle_wait alone, so it never traps.
kickos_add_kseam_gate(NAME wild_stack SOURCES wild_stack.cc)
