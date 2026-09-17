# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# What a fused reply-receive owes on its way out, over kernel/syscall/syscall_ipc.cc itself.
#
# THE K-SEAM SOURCES AT TWO KERNEL CORES, which is the whole point for the announcement arms:
# the obligation they pin is one core telling another to look, and at one core
# poke_peers_below compiles to nothing and every arm would pass having witnessed nothing. The
# sim preset drives one core, so this gate cannot join kickos_kseam; exitquiesce's
# kickos_kseam_smp is the same sources at the right posture, and this library is that set plus
# the translation unit under test.
#
# ipc_seam.cc answers the six symbols that set leaves undefined, all of them in the address
# layer. It is NOT syscall_mem.cc: that file reads MPU and address-space state the host
# fixture seats no board for, and the arms here own their buffers outright.
#
# KICKOS_BENCH=1 IS AN INSTRUMENT AND NOT A POSTURE HERE. The brackets compile to nothing
# without it, and one of them is the boundary an arm below holds: PH_REPLY_RECV_TOTAL must
# close PAST the deferred wake's seat. bench_phase_add is an ordinary out-of-line call and
# kernel/bench/bench.cc is not in this set, so bench_seam.cc answers it and records WHEN the
# row was taken. It answers the four further symbols the bench build pulls in as well.
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
