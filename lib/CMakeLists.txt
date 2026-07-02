# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

add_library(kickos_lib STATIC
  libc/string.cc
  libc/fmt.cc)
kickos_apply_freestanding(kickos_lib)
target_include_directories(kickos_lib PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
