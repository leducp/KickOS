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
target_include_directories(kickos_user PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(kickos_user PUBLIC kickos_arch_${KICKOS_ARCH} kickos_lib)
