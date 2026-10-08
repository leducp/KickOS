# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Test fused reply-receive using the real IPC and scheduler code at two cores.
# Peer notifications are compiled out on single-core builds.
# ipc_seam supplies address validation and copying without board MPU state.
# Enable benchmark calls so bench_seam can record whether REPLY_RECV_TOTAL
# closes after deferred wake completion.
kickos_add_kseam(kickos_kseam_ipc_smp
  EXTRA ipc_seam.cc bench_seam.cc "${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_ipc.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/syscall"
  POSTURE KICKOS_NUM_CORES=2 KICKOS_KERNEL_CORES=2 KICKOS_MULTICORE_MODEL_SHARED=1
          KICKOS_BENCH=1 KICKOS_BENCH_SCHED=1)

kickos_add_unit_test(NAME fused_wake SOURCES owed_set.cc
  LIBRARIES kickos_kseam_ipc_smp)

# Includes bench.cc, so it links no K-seam: bench_seam.cc defines the same symbols.
kickos_add_unit_test(NAME bench_e2e_publish SOURCES e2e_publish.cc
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/bench"
  POSTURE KICKOS_NUM_CORES=2 KICKOS_KERNEL_CORES=2 KICKOS_MULTICORE_MODEL_SHARED=1
          KICKOS_BENCH=1)
