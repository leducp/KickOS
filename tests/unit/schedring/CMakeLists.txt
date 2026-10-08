# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The per-pair rings a thread readied toward a peer crosses, over the K-seam sources AT TWO KERNEL
# CORES. Those are already compiled by tests/unit/exitquiesce as kickos_kseam_smp, whose target
# must therefore exist before this directory is added.
kickos_add_unit_test(NAME sched_ring SOURCES ring.cc
  LIBRARIES kickos_kseam_smp)
# ring.cc counts re-seats from between the scheduler and the policy.
target_link_options(sched_ring PRIVATE "LINKER:--wrap=_ZN6kickos16policy_on_removeEPNS_6ThreadE")

# The same sources AT THREE KERNEL CORES, for the arms that need two placers and a third core.
kickos_add_kseam(kickos_kseam_smp3
  POSTURE KICKOS_NUM_CORES=3 KICKOS_KERNEL_CORES=3 KICKOS_MULTICORE_MODEL_SHARED=1)

kickos_add_unit_test(NAME sched_ring3 SOURCES ring3.cc
  LIBRARIES kickos_kseam_smp3)
