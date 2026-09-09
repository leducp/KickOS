# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The per-task object budget, over a posture the preset cannot supply. THREE macros, and each
# is load-bearing for a different arm:
#   KICKOS_TASK_OBJECT_BUDGET=2  below the semaphore pool's slots-minus-reserve, so the
#                                BUDGET half of the ceiling is what refuses there
#   KICKOS_MAX_SEMAPHORES=8      wide enough that a task at its ceiling leaves slots free, so
#                                an arm can tell a budget refusal from an exhausted pool, and
#                                that two tasks at their ceilings both fit
#   KICKOS_MAX_MUTEXES=2         slots-minus-reserve BELOW the budget, so the RESERVE half is
#                                what refuses there. Without a pool this narrow, dropping the
#                                reserve from the ceiling would redden nothing.
# The pool sizes size `struct Kernel`, so they cannot be a bare DEFINITIONS on a target
# linking the shared kickos_kseam: the fixture and the arm would disagree on the layout.
kickos_add_kernel_host_lib(kickos_kseam_budget
  SOURCES
    "${PROJECT_SOURCE_DIR}/tests/unit/kfixture/kfixture.cc"
    "${PROJECT_SOURCE_DIR}/tests/unit/kfixture/karch_seam.cc"
    "${PROJECT_SOURCE_DIR}/kernel/sched/sched.cc"
    "${PROJECT_SOURCE_DIR}/kernel/sched/policy_fifo_rr.cc"
    "${PROJECT_SOURCE_DIR}/kernel/sync/sync.cc"
    "${PROJECT_SOURCE_DIR}/kernel/thread/park.cc"
    "${PROJECT_SOURCE_DIR}/kernel/syscall/cap.cc"
    "${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_obj.cc"
    "${PROJECT_SOURCE_DIR}/kernel/task/task.cc"
    "${PROJECT_SOURCE_DIR}/kernel/irq/irq.cc"
    "${PROJECT_SOURCE_DIR}/kernel/init/instance.cc"
    "${PROJECT_SOURCE_DIR}/kernel/init/fault.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/tests/unit/kfixture"
  POSTURE KICKOS_TASK_OBJECT_BUDGET=2 KICKOS_MAX_SEMAPHORES=8 KICKOS_MAX_MUTEXES=2)

# syscall_obj.cc is in that set and not in kickos_kseam's: sem_create and mutex_create are
# two of the four charged creators, and the third (endpoint_create) sits in syscall_ipc.cc
# with the whole far-IPC path behind it. The endpoint creator's refusal is a board arm.
kickos_add_unit_test(NAME task_budget SOURCES object_budget.cc
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/syscall"
  LIBRARIES kickos_kseam_budget)
