# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Build the x86-64 map editor on the host with a recorded machine.
# Use two cores to exercise the peer rendezvous.
set(_mapshoot_backend "${PROJECT_SOURCE_DIR}/arch/x86/x86_64/aspace_x86_64.cc")
if(NOT EXISTS "${_mapshoot_backend}")
  message(FATAL_ERROR
    "KickOS: ${_mapshoot_backend} is gone, so map_shoot would witness nothing. This gate "
    "compiles the x86_64 backend's own translation unit; a moved file breaks it silently.")
endif()

kickos_add_kernel_host_lib(kickos_aspace_x86_64_host
  SOURCES "${_mapshoot_backend}" x86_64_sysops_seam.cc
          "${PROJECT_SOURCE_DIR}/tests/unit/common/sysops_seam.cc"
  INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/tests/unit/common"
           "${PROJECT_SOURCE_DIR}/arch/x86/x86_64/include"
           "${PROJECT_SOURCE_DIR}/arch/x86/chip/q35/include"
  POSTURE KICKOS_NUM_CORES=2 KICKOS_KERNEL_CORES=2 KICKOS_MULTICORE_MODEL_SHARED=1
          KICKOS_HAVE_ASPACE=1 KICKOS_X86_64_SYSOPS_FROM_SEAM=1)
kickos_add_unit_test(NAME map_shoot SOURCES map_shoot.cc
  LIBRARIES kickos_aspace_x86_64_host)

kickos_add_unit_test(NAME pe_sections SOURCES pe_sections.cc
  INCLUDES "${PROJECT_SOURCE_DIR}/arch/x86/x86_64/include")
