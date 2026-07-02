# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

if(KICKOS_ARCH STREQUAL "sim")
  add_library(kickos_arch_sim STATIC sim/sim.cc sim/start.cc)
  kickos_apply_hosted(kickos_arch_sim)
  target_include_directories(kickos_arch_sim PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/sim/include")
  # Kernel headers for the reverse-direction callbacks are pulled via arch.h.
else()
  message(FATAL_ERROR "KickOS: arch backend '${KICKOS_ARCH}' not available in M0")
endif()
