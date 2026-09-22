# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The K-seam fixture: the kernel's stateful core compiled for the HOST, with karch_seam.cc
# answering the arch boundary. AT THE PRESET'S OWN POSTURE, which is what makes it the
# fixture the ordinary gates share; the postured variants are their own libraries.
#
# Nothing in the tree tests __STDC_HOSTED__ or __EXCEPTIONS today, so the preprocessed
# kernel TUs are identical to the shipping build's; an #if on one of those in a kernel
# header would silently diverge the gate from the image.
kickos_add_kernel_host_lib(kickos_kseam
  SOURCES
    kfixture.cc
    karch_seam.cc
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
  INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}")
