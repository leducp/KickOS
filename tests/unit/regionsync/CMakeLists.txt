# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The cache maintenance a region chip owes a block when a region of another memory type than its
# last one is seated over it, at the three seats: a self-grant, and a new thread's windows, task
# data and stack. Over the REAL kernel/thread/thread.cc, kernel/syscall/syscall_mem.cc,
# kernel/grant/grant.cc and kernel/mem/ramown.cc at armv7m's KICKOS_ARCH_ARENA_DCACHE, the arch's
# maintenance counted in its place. Linked with --gc-sections: only the seats' own callees need
# a seam.
kickos_add_unit_test(NAME region_sync
  SOURCES region_sync.cc
          "${PROJECT_SOURCE_DIR}/kernel/thread/thread.cc"
          "${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_mem.cc"
          "${PROJECT_SOURCE_DIR}/kernel/grant/grant.cc"
          "${PROJECT_SOURCE_DIR}/kernel/mem/ramown.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/syscall"
  POSTURE KICKOS_ARCH_ARENA_DCACHE=1)
target_compile_options(region_sync PRIVATE -ffunction-sections -fdata-sections)
target_link_options(region_sync PRIVATE -Wl,--gc-sections)
set_source_files_properties("${PROJECT_SOURCE_DIR}/kernel/thread/thread.cc"
                            "${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_mem.cc"
                            "${PROJECT_SOURCE_DIR}/kernel/grant/grant.cc"
                            "${PROJECT_SOURCE_DIR}/kernel/mem/ramown.cc"
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  PROPERTIES COMPILE_OPTIONS "-ffreestanding;-fno-exceptions;-fno-rtti;-fno-threadsafe-statics")
