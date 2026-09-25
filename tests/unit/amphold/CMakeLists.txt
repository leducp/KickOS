# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The amp hold: a far call's receiver and a far reply's caller land the payload out of the held
# ring slot in their own syscall, and every way either leaves gives the slot back. Over the
# real kernel/syscall/syscall_ipc.cc and kernel/amp at an own-image node's posture, with the
# register fastpath forced in as the RP2350 nodes carry it.
#
# ONE SOURCE SET, NOT kickos_kseam's: that library takes the preset's posture, where the whole
# amp window compiles to nothing. ipc_seam.cc is the user-access seam fused_wake already uses.
set(_amphold_srcs
  "${PROJECT_SOURCE_DIR}/tests/unit/kfixture/kfixture.cc"
  "${PROJECT_SOURCE_DIR}/tests/unit/kfixture/karch_seam.cc"
  "${PROJECT_SOURCE_DIR}/tests/unit/fusedwake/ipc_seam.cc"
  amp_hold_seam.cc
  "${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_ipc.cc"
  "${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_ipc_fast.cc"
  "${PROJECT_SOURCE_DIR}/kernel/amp/ampwindow.cc"
  "${PROJECT_SOURCE_DIR}/kernel/amp/ampmap.cc"
  "${PROJECT_SOURCE_DIR}/kernel/sched/sched.cc"
  "${PROJECT_SOURCE_DIR}/kernel/sched/policy_fifo_rr.cc"
  "${PROJECT_SOURCE_DIR}/kernel/sync/sync.cc"
  "${PROJECT_SOURCE_DIR}/kernel/thread/park.cc"
  "${PROJECT_SOURCE_DIR}/kernel/syscall/cap.cc"
  "${PROJECT_SOURCE_DIR}/kernel/task/task.cc"
  "${PROJECT_SOURCE_DIR}/kernel/irq/irq.cc"
  "${PROJECT_SOURCE_DIR}/kernel/notify/notify.cc"
  "${PROJECT_SOURCE_DIR}/kernel/init/instance.cc"
  "${PROJECT_SOURCE_DIR}/kernel/init/fault.cc")

kickos_add_kernel_host_lib(kickos_kseam_amp
  SOURCES ${_amphold_srcs}
  INCLUDES "${PROJECT_SOURCE_DIR}/tests/unit/kfixture"
           "${PROJECT_SOURCE_DIR}/tests/unit/fusedwake"
           "${PROJECT_SOURCE_DIR}/kernel/syscall"
  POSTURE KICKOS_ARCH_HAS_IPC_FASTPATH=1
          KICKOS_NUM_CORES=1
          KICKOS_KERNEL_CORES=1
          KICKOS_MULTICORE_MODEL_SHARED=0
          KICKOS_AMP_NODE=1
          KICKOS_AMP_HOLD_COPY_HOOK=1
          KICKOS_AMP_OWN_IMAGE=1
          KICKOS_AMP_NODES=2
          KICKOS_AMP_NODE_ID=0
          KICKOS_AMP_SELF_NODE=0
          KICKOS_AMP_PORT_COUNT=2
          "KICKOS_AMP_PORT_NODE_LIST=0,1"
          "KICKOS_AMP_PORT_PORT_LIST=2,3"
          "KICKOS_AMP_NODE_CORE_LIST={0,1}")

kickos_add_unit_test(NAME amp_hold SOURCES hold.cc
  LIBRARIES kickos_kseam_amp)
