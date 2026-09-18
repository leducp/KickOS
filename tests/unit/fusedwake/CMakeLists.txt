# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Test fused reply-receive using the real IPC and scheduler code at two cores.
# Peer notifications are compiled out on single-core builds.
# ipc_seam supplies address validation and copying without board MPU state.
# Enable benchmark calls so bench_seam can record whether REPLY_RECV_TOTAL
# closes after deferred wake completion.
set(_fusedwake_tu "${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_ipc.cc")
if(NOT EXISTS "${_fusedwake_tu}")
  message(FATAL_ERROR
    "KickOS: ${_fusedwake_tu} is gone, so fused_wake would witness nothing.")
endif()

kickos_add_kernel_host_lib(kickos_kseam_ipc_smp
  SOURCES
    "${PROJECT_SOURCE_DIR}/tests/unit/kfixture/kfixture.cc"
    "${PROJECT_SOURCE_DIR}/tests/unit/kfixture/karch_seam.cc"
    ipc_seam.cc
    bench_seam.cc
    "${_fusedwake_tu}"
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
           "${PROJECT_SOURCE_DIR}/kernel/syscall"
  POSTURE KICKOS_NUM_CORES=2 KICKOS_KERNEL_CORES=2 KICKOS_MULTICORE_MODEL_SHARED=1
          KICKOS_BENCH=1)

kickos_add_unit_test(NAME fused_wake SOURCES owed_set.cc
  LIBRARIES kickos_kseam_ipc_smp)
