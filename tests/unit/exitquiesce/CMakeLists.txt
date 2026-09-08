# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The exited-slot reclaim window. The K-seam sources again, AT TWO KERNEL CORES and with
# kernel/sync/klock.cc beside them, which is why this gate builds its own posture library
# rather than joining kickos_kseam: that library takes the preset's core count, and the sim
# preset drives one core, where every body of the lock compiles to nothing and the span this
# gate asserts is not expressible.
#
# SHARED: tests/unit/migrateask and tests/unit/slaypeer link it, so both are added after
# this directory.
kickos_add_kernel_host_lib(kickos_kseam_smp
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
    "${PROJECT_SOURCE_DIR}/kernel/init/instance.cc"
    "${PROJECT_SOURCE_DIR}/kernel/init/fault.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/tests/unit/kfixture"
  POSTURE KICKOS_NUM_CORES=2 KICKOS_KERNEL_CORES=2 KICKOS_MULTICORE_MODEL_SHARED=1)

kickos_add_unit_test(NAME exit_quiesce SOURCES exit_span.cc
  LIBRARIES kickos_kseam_smp)
