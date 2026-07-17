# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

set(KICKOS_USER_SOURCES src/syscall_stubs.cc)

# The newlib porting layer clashes with host libc on the sim; it is linked from
# M1 when userspace rides the ARM toolchain libc.
if(NOT KICKOS_ARCH STREQUAL "sim")
  list(APPEND KICKOS_USER_SOURCES src/newlib_stubs.cc)
endif()

add_library(kickos_user STATIC ${KICKOS_USER_SOURCES})
kickos_apply_freestanding(kickos_user)
# Under RISC-V PMP enforcement, app/user globals must land in the .appdata NAPOT
# region (linker `*user*(.data/.bss)`) and be addressed absolutely -- NOT in the
# gp-relative .sdata/.sbss small-data window, which lives with the kernel around
# __global_pointer$ and is unreachable by an unprivileged thread. -msmall-data-limit=0
# routes every global to .data/.bss so it is captured by the app-data region.
if(KICKOS_ARCH STREQUAL "rv32imac" AND KICKOS_HAVE_MPU)
  target_compile_options(kickos_user PRIVATE -msmall-data-limit=0)
endif()
target_include_directories(kickos_user PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(kickos_user PUBLIC kickos_arch_${KICKOS_ARCH} kickos_lib)
install(TARGETS kickos_user EXPORT KickOSTargets
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
