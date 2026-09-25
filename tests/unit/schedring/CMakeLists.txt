# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The per-pair rings a thread readied toward a peer crosses, over the K-seam sources AT TWO KERNEL
# CORES. Those are already compiled by tests/unit/exitquiesce as kickos_kseam_smp, whose target
# must therefore exist before this directory is added.
kickos_add_unit_test(NAME sched_ring SOURCES ring.cc
  LIBRARIES kickos_kseam_smp)

# The same sources AT THREE KERNEL CORES, for the arms that need two placers and a third core.
kickos_add_kernel_host_lib(kickos_kseam_smp3
  SOURCES
    "${PROJECT_SOURCE_DIR}/tests/unit/kfixture/kfixture.cc"
    "${PROJECT_SOURCE_DIR}/tests/unit/kfixture/karch_seam.cc"
    "${PROJECT_SOURCE_DIR}/kernel/sched/sched.cc"
    "${PROJECT_SOURCE_DIR}/kernel/sched/policy_fifo_rr.cc"
    "${PROJECT_SOURCE_DIR}/kernel/sync/sync.cc"
    "${PROJECT_SOURCE_DIR}/kernel/sync/klock.cc"
    "${PROJECT_SOURCE_DIR}/kernel/thread/park.cc"
    "${PROJECT_SOURCE_DIR}/kernel/syscall/cap.cc"
    "${PROJECT_SOURCE_DIR}/kernel/task/task.cc"
    "${PROJECT_SOURCE_DIR}/kernel/irq/irq.cc"
    "${PROJECT_SOURCE_DIR}/kernel/notify/notify.cc"
    "${PROJECT_SOURCE_DIR}/kernel/init/instance.cc"
    "${PROJECT_SOURCE_DIR}/kernel/init/fault.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/tests/unit/kfixture"
  POSTURE KICKOS_NUM_CORES=3 KICKOS_KERNEL_CORES=3 KICKOS_MULTICORE_MODEL_SHARED=1)

kickos_add_unit_test(NAME sched_ring3 SOURCES ring3.cc
  LIBRARIES kickos_kseam_smp3)
