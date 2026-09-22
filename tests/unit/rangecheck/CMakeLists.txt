# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The user-pointer range checks, compiled from the shipping translation unit against the
# K-seam fixture's real scheduler and real thread pool. The arch static-extent hooks are
# answered by the gate source, which is what lets an arm move the granted set and the
# fallback independently.
#
# TWICE, at both postures the checks ship at. At the preset's own posture the whole
# KICKOS_HAVE_ASPACE half of the merged walk is preprocessed away, and that half is what the
# three translating arches run: a gate reading only this one witnesses an equivalence claim
# over source text the compiler never saw.
kickos_add_unit_test(NAME range_check
  SOURCES range_check.cc "${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_mem.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/syscall" "${CMAKE_CURRENT_SOURCE_DIR}"
  LIBRARIES kickos_kseam)

# THE SAME SOURCE AT THE TRANSLATING POSTURE. The range list the checks consult lives in the
# domain, so the K-seam sources are compiled again with KICKOS_HAVE_ASPACE=1 and the real
# VirtualRanges comes with them; aspace_seam.cc stands in for kernel/domain and kernel/mem,
# which no board the host arch answers for provides.
set(_rc_kseam
  "${PROJECT_SOURCE_DIR}/tests/unit/kfixture/kfixture.cc"
  "${PROJECT_SOURCE_DIR}/tests/unit/kfixture/karch_seam.cc"
  aspace_seam.cc
  "${PROJECT_SOURCE_DIR}/kernel/mem/vrange.cc"
  "${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_mem.cc"
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

kickos_add_kernel_host_lib(kickos_kseam_rangecheck_aspace
  SOURCES ${_rc_kseam}
  INCLUDES "${PROJECT_SOURCE_DIR}/tests/unit/kfixture"
           "${PROJECT_SOURCE_DIR}/kernel/syscall" "${CMAKE_CURRENT_SOURCE_DIR}"
  POSTURE KICKOS_HAVE_ASPACE=1)
kickos_add_unit_test(NAME range_check_aspace SOURCES range_check.cc
  TEST_PREFIX aspace_
  LIBRARIES kickos_kseam_rangecheck_aspace)
