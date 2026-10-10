# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Where a READY thread is placed across cores, what a core whose level falls asks for, and the
# cross-core ask a re-placement owes (migrate_ask.cc), over the K-seam sources AT TWO KERNEL
# CORES. Those are already compiled by tests/unit/exitquiesce as kickos_kseam_smp, whose target
# must therefore exist before this directory is added.
kickos_add_unit_test(NAME placement SOURCES placement.cc migrate_ask.cc
  LIBRARIES kickos_kseam_smp)
# placement.cc watches the decline walk from between sched.cc and the policy.
target_link_options(placement PRIVATE
  "LINKER:--wrap=_ZN6kickos15policy_declinedENS_10KernelCoreEiPNS_9SchedWalkE")

# The placement reads over the real kernel/syscall/syscall_thread.cc at two kernel cores. Linked
# with --gc-sections: only the reads' own callees, which the K-seam already holds, are linked.
kickos_add_unit_test(NAME placement_read
  SOURCES placement_read.cc "${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_thread.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/syscall"
  LIBRARIES kickos_kseam_smp)
target_compile_options(placement_read PRIVATE -ffunction-sections -fdata-sections)
target_link_options(placement_read PRIVATE -Wl,--gc-sections)
set_source_files_properties("${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_thread.cc"
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  PROPERTIES COMPILE_OPTIONS "-ffreestanding;-fno-exceptions;-fno-rtti;-fno-threadsafe-statics")
