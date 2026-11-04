# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The amp hold: a far call's receiver and a far reply's caller land the payload out of the held
# ring slot in their own syscall, and every way either leaves gives the slot back. Over the
# real kernel/syscall/syscall_ipc.cc and kernel/amp at an own-image node's posture, with the
# register fastpath forced in as the RP2350 nodes carry it.
#
# ONE SOURCE SET, NOT kickos_kseam's: that library takes the preset's posture, where the whole
# amp window compiles to nothing. ipc_seam.cc is the user-access seam fused_wake already uses.
kickos_add_kseam(kickos_kseam_amp
  EXTRA "${PROJECT_SOURCE_DIR}/tests/unit/fusedwake/ipc_seam.cc"
        amp_hold_seam.cc
        "${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_ipc.cc"
        "${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_ipc_fast.cc"
        ampwindow_shim.cc
        "${PROJECT_SOURCE_DIR}/kernel/amp/ampmap.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/tests/unit/fusedwake"
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

# The endpoint badge's mint and its delivery on the local, fastpath and far paths, over the same
# source set.
kickos_add_unit_test(NAME ipc_badge SOURCES badge.cc stale_reply.cc
  LIBRARIES kickos_kseam_amp)

# Every case of each binary in one process, in shuffled orders: no case leans on what another
# left in the amp window.
foreach(_bin amp_hold ipc_badge)
  add_test(NAME ${_bin}_one_process COMMAND ${_bin} --gtest_shuffle --gtest_repeat=20)
  kickos_host_gate(${_bin}_one_process TIMEOUT 60)
  set_property(TEST ${_bin}_one_process APPEND PROPERTY FIXTURES_REQUIRED kickos_build)
endforeach()
