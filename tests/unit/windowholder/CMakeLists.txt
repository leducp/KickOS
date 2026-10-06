# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Who holds a device window, over the REAL kernel/thread/thread.cc: the one-holder rule and the
# holder outside a task the console publish asks for. Linked with --gc-sections: only the two
# queries' own callees need a seam.
kickos_add_unit_test(NAME window_holder
  SOURCES window_holder.cc "${PROJECT_SOURCE_DIR}/kernel/thread/thread.cc"
  KERNEL)
target_compile_options(window_holder PRIVATE -ffunction-sections -fdata-sections)
target_link_options(window_holder PRIVATE -Wl,--gc-sections)
set_source_files_properties("${PROJECT_SOURCE_DIR}/kernel/thread/thread.cc"
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  PROPERTIES COMPILE_OPTIONS "-ffreestanding;-fno-exceptions;-fno-rtti;-fno-threadsafe-statics")
