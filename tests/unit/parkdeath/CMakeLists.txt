# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A cancel honoured at a park prologue ends the park's bracket before the capability sweep.
# Host-only of necessity: the prologue is reached only by a cancel landing between syscall
# entry and the bracket, which no image can arrange, and the witness is the seam's bracket
# count at each chunk boundary.
#
# Needs kickos_kseam_smp (tests/unit/exitquiesce) and kickos_kseam_ipc_smp
# (tests/unit/fusedwake) to exist before this directory is added.
kickos_add_unit_test(NAME cancelled_park SOURCES sync_park.cc
  LIBRARIES kickos_kseam)
kickos_add_unit_test(NAME cancelled_park_smp SOURCES sync_park.cc
  LIBRARIES kickos_kseam_smp
  TEST_PREFIX smp.)

# The K-seam with the real time.cc in place of the seam's ktime stubs.
kickos_add_kernel_host_lib(kickos_kseam_time
  SOURCES
    "${PROJECT_SOURCE_DIR}/tests/unit/kfixture/kfixture.cc"
    "${PROJECT_SOURCE_DIR}/tests/unit/kfixture/karch_seam.cc"
    "${PROJECT_SOURCE_DIR}/kernel/time/time.cc"
    "${PROJECT_SOURCE_DIR}/kernel/sched/sched.cc"
    "${PROJECT_SOURCE_DIR}/kernel/sched/policy_fifo_rr.cc"
    "${PROJECT_SOURCE_DIR}/kernel/sync/sync.cc"
    "${PROJECT_SOURCE_DIR}/kernel/thread/park.cc"
    "${PROJECT_SOURCE_DIR}/kernel/syscall/cap.cc"
    "${PROJECT_SOURCE_DIR}/kernel/task/task.cc"
    "${PROJECT_SOURCE_DIR}/kernel/irq/irq.cc"
    "${PROJECT_SOURCE_DIR}/kernel/notify/notify.cc"
    "${PROJECT_SOURCE_DIR}/kernel/init/instance.cc"
    "${PROJECT_SOURCE_DIR}/kernel/init/fault.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/tests/unit/kfixture")
target_compile_definitions(kickos_kseam_time PRIVATE KFIXTURE_REAL_TIME=1)
kickos_add_unit_test(NAME cancelled_timed_park SOURCES timed_park.cc
  LIBRARIES kickos_kseam_time)

kickos_add_unit_test(NAME cancelled_ipc_park SOURCES ipc_park.cc
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/syscall"
  LIBRARIES kickos_kseam_ipc_smp)
