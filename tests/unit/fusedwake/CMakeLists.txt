# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Test fused reply-receive using the real IPC and scheduler code at two cores.
# Peer notifications are compiled out on single-core builds.
# ipc_seam supplies address validation and copying without board MPU state.
kickos_add_kseam(kickos_kseam_ipc_smp
  EXTRA ipc_seam.cc "${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_ipc.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/syscall"
  POSTURE KICKOS_NUM_CORES=2 KICKOS_KERNEL_CORES=2 KICKOS_MULTICORE_MODEL_SHARED=1
          KICKOS_MEMORY_ENFORCED=1 ARCH_MPU_RO_KEEPS_KERNEL_WRITE=0)

kickos_add_unit_test(NAME fused_wake SOURCES owed_set.cc
  LIBRARIES kickos_kseam_ipc_smp)

# A watch waiter's lend over the real IPC and scheduler at two cores.
kickos_add_unit_test(NAME watch_lend_ipc SOURCES watch_lend.cc
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/syscall"
  LIBRARIES kickos_kseam_ipc_smp)
