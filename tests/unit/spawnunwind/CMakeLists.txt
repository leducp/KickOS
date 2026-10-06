# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# What a spawn its region set refuses hands back: over the real kernel/syscall/syscall_thread.cc
# and kernel/thread/thread.cc on the K-seam, spawn_unwind.cc answering the arena, the domain
# regions and the user copy. Linked with --gc-sections: only the spawn's own callees need a seam.
kickos_add_unit_test(NAME spawn_unwind
  SOURCES spawn_unwind.cc
          "${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_thread.cc"
          "${PROJECT_SOURCE_DIR}/kernel/thread/thread.cc"
          "${PROJECT_SOURCE_DIR}/kernel/grant/grant.cc"
          "${PROJECT_SOURCE_DIR}/kernel/mem/ramown.cc"
          "${PROJECT_SOURCE_DIR}/kernel/thread/tls.cc"
  INCLUDES "${PROJECT_SOURCE_DIR}/kernel/syscall"
  LIBRARIES kickos_kseam)
target_compile_options(spawn_unwind PRIVATE -ffunction-sections -fdata-sections)
target_link_options(spawn_unwind PRIVATE -Wl,--gc-sections)
set_source_files_properties("${PROJECT_SOURCE_DIR}/kernel/syscall/syscall_thread.cc"
                            "${PROJECT_SOURCE_DIR}/kernel/thread/thread.cc"
                            "${PROJECT_SOURCE_DIR}/kernel/grant/grant.cc"
                            "${PROJECT_SOURCE_DIR}/kernel/mem/ramown.cc"
                            "${PROJECT_SOURCE_DIR}/kernel/thread/tls.cc"
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  PROPERTIES COMPILE_OPTIONS "-ffreestanding;-fno-exceptions;-fno-rtti;-fno-threadsafe-statics")
