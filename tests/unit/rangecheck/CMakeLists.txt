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
# VirtualRanges comes with them. kernel/mem/aspace.cc is compiled whole and the link keeps what
# the switch and exit paths reach; aspace_seam.cc stands in for kernel/domain and the arch
# boundary. KICKOS_ARCH_ALIAS_DCACHE is armv8a's and x86_64's posture, and dcache_seam.cc
# records the maintenance it compiles.
set_source_files_properties("${PROJECT_SOURCE_DIR}/kernel/mem/aspace.cc"
                            "${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_mem.cc"
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  PROPERTIES COMPILE_OPTIONS "-ffunction-sections;-fdata-sections")
kickos_add_kseam(kickos_kseam_rangecheck_aspace
  EXTRA aspace_seam.cc dcache_seam.cc
        "${PROJECT_SOURCE_DIR}/kernel/mem/aspace.cc"
        "${PROJECT_SOURCE_DIR}/kernel/mem/vrange.cc"
        "${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_mem.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/syscall" "${CMAKE_CURRENT_SOURCE_DIR}"
           "${PROJECT_SOURCE_DIR}/tests/unit/common"
  POSTURE KICKOS_HAVE_ASPACE=1 KICKOS_ARCH_ALIAS_DCACHE=1 KICKOS_ARCH_IRQ_WINDOW=1)
kickos_add_unit_test(NAME range_check_aspace SOURCES range_check.cc
  TEST_PREFIX aspace_
  LIBRARIES kickos_kseam_rangecheck_aspace)
target_link_options(range_check_aspace PRIVATE -Wl,--gc-sections)

# The cache maintenance the copy seam brackets a non-cacheable user end with, at both postures.
# The region one compiles the K-seam sources again at armv7m's KICKOS_ARCH_ARENA_DCACHE, so no
# translation unit of the gate sees the other posture's definitions.
kickos_add_kseam(kickos_kseam_alias_region
  EXTRA dcache_seam.cc "${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_mem.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/syscall" "${CMAKE_CURRENT_SOURCE_DIR}"
  POSTURE KICKOS_ARCH_ARENA_DCACHE=1)
kickos_add_unit_test(NAME alias_sync SOURCES alias_sync.cc
  LIBRARIES kickos_kseam_alias_region)
kickos_add_unit_test(NAME alias_sync_aspace SOURCES alias_sync.cc split_access.cc
  TEST_PREFIX aspace_
  LIBRARIES kickos_kseam_rangecheck_aspace)
target_link_options(alias_sync_aspace PRIVATE -Wl,--gc-sections)
