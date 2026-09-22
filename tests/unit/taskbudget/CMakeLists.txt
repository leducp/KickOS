# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The per-task object budget, over a posture the preset cannot supply. FIVE macros, and each
# is load-bearing for a different arm:
#   KICKOS_TASK_SEMAPHORE_BUDGET=2  the ceiling most arms count creates against by hand
#   KICKOS_MAX_SEMAPHORES=8         wide enough that a task at its ceiling leaves slots free,
#                                   so an arm can tell a budget refusal from an exhausted
#                                   pool, and that two tasks at their ceilings both fit
#   KICKOS_TASK_MUTEX_BUDGET=1      a DIFFERENT ceiling on a second pool, so an arm reddens if
#                                   one figure ever stands for all four pools again
#   KICKOS_MAX_MUTEXES=4            and that pool stays wider than its budget, so the mutex
#                                   refusal cannot be exhaustion either
#   KICKOS_TASK_ENDPOINT_BUDGET=2   two endpoint holds is what the recycled-slot arm counts
#   KICKOS_TASK_IRQ_HANDLE_BUDGET=3 a THIRD distinct ceiling, so the IRQ arm reddens if the
#                                   IRQ kind ever resolves to another pool's figure
#   KICKOS_MAX_IRQ_HANDLES=6        wider than that budget, so the IRQ refusal cannot be an
#                                   exhausted binding pool either
#   KICKOS_TASK_NOTIFY_BUDGET=4     a FOURTH distinct ceiling, so the notification arm reddens
#                                   if that kind ever resolves to another pool's figure
#   KICKOS_MAX_NOTIFY=7             wider than that budget, so its refusal cannot be an
#                                   exhausted notification pool either
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
    "${PROJECT_SOURCE_DIR}/kernel/notify/notify.cc"
    "${PROJECT_SOURCE_DIR}/kernel/init/instance.cc"
    "${PROJECT_SOURCE_DIR}/kernel/init/fault.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/tests/unit/kfixture"
  POSTURE KICKOS_TASK_SEMAPHORE_BUDGET=2 KICKOS_MAX_SEMAPHORES=8
          KICKOS_TASK_MUTEX_BUDGET=1 KICKOS_MAX_MUTEXES=4
          KICKOS_TASK_ENDPOINT_BUDGET=2
          KICKOS_TASK_IRQ_HANDLE_BUDGET=3 KICKOS_MAX_IRQ_HANDLES=6
          KICKOS_TASK_NOTIFY_BUDGET=4 KICKOS_MAX_NOTIFY=7)

# syscall_obj.cc is in that set and not in kickos_kseam's: sem_create and mutex_create are
# two of the four charged creators, and the third (endpoint_create) sits in syscall_ipc.cc
# with the whole far-IPC path behind it. The endpoint creator's refusal is a board arm.
kickos_add_unit_test(NAME task_budget SOURCES object_budget.cc
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/syscall"
  LIBRARIES kickos_kseam_budget)
