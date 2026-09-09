# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The fastpath's decide-before-mutate rule, over kernel/syscall/syscall_ipc_fast.cc itself.
#
# THE K-SEAM SOURCES AGAIN, with KICKOS_ARCH_HAS_IPC_FASTPATH FORCED TO 1: the knob is an arch
# backend's own declaration (arch/<family>/<arch>/ipc_fastpath.cmake) and takes no -D in either
# direction, no board the host arch answers for ships that file, and so the whole translation
# unit under test compiles to nothing at the preset's posture. The force goes through the
# POSTURE scrub and not a second -D.
#
# KICKOS_KERNEL_CORES STAYS 1 and is named rather than inherited: the file carries a
# static_assert on it, so this library is also what proves that assert is silent at the one
# posture the fastpath is admitted at.
#
# syscall_mem.cc comes along because ep_copy and write_recv_info are the fastpath's only calls
# into the memory layer and its refusal is one of the paths below; fast_seam.cc answers what
# that file and the forced arm leave undefined.
set(_fastrefuse_srcs
  "${PROJECT_SOURCE_DIR}/tests/unit/kfixture/kfixture.cc"
  "${PROJECT_SOURCE_DIR}/tests/unit/kfixture/karch_seam.cc"
  fast_seam.cc
  "${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_ipc_fast.cc"
  "${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_mem.cc"
  "${PROJECT_SOURCE_DIR}/kernel/sched/sched.cc"
  "${PROJECT_SOURCE_DIR}/kernel/sched/policy_fifo_rr.cc"
  "${PROJECT_SOURCE_DIR}/kernel/sync/sync.cc"
  "${PROJECT_SOURCE_DIR}/kernel/thread/park.cc"
  "${PROJECT_SOURCE_DIR}/kernel/syscall/cap.cc"
  "${PROJECT_SOURCE_DIR}/kernel/task/task.cc"
  "${PROJECT_SOURCE_DIR}/kernel/irq/irq.cc"
  "${PROJECT_SOURCE_DIR}/kernel/init/instance.cc"
  "${PROJECT_SOURCE_DIR}/kernel/init/fault.cc")

# A moved file would leave this gate compiling a fastpath that is not the shipping one, and
# the forced arm would then be nothing at all.
set(_fastrefuse_tu "${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_ipc_fast.cc")
if(NOT EXISTS "${_fastrefuse_tu}")
  message(FATAL_ERROR
    "KickOS: ${_fastrefuse_tu} is gone, so fast_refuse would witness nothing.")
endif()

kickos_add_kernel_host_lib(kickos_kseam_fastpath
  SOURCES ${_fastrefuse_srcs}
  INCLUDES "${PROJECT_SOURCE_DIR}/tests/unit/kfixture"
           "${PROJECT_SOURCE_DIR}/kernel/syscall"
  POSTURE KICKOS_ARCH_HAS_IPC_FASTPATH=1
          KICKOS_NUM_CORES=1
          KICKOS_KERNEL_CORES=1
          KICKOS_MULTICORE_MODEL_SHARED=0)

kickos_add_unit_test(NAME fast_refuse SOURCES refuse_untouched.cc
  LIBRARIES kickos_kseam_fastpath)
