# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Build the RV64 map editor on the host with recorded fences and peer masks.
# Use Sv39 (satp.MODE=8) and two cores to exercise peer synchronization.
# The system-operation seam replaces privileged instructions.
set(_mapfence_backend "${PROJECT_SOURCE_DIR}/arch/riscv/rv64imac/aspace_rv64imac.cc")
if(NOT EXISTS "${_mapfence_backend}")
  message(FATAL_ERROR
    "KickOS: ${_mapfence_backend} is gone, so map_fence would witness nothing. This gate "
    "compiles the rv64 backend's own translation unit; a moved file breaks it silently.")
endif()

kickos_add_kernel_host_lib(kickos_aspace_rv64imac_host
  SOURCES "${_mapfence_backend}" rv64_sysops_seam.cc
  INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}"
           "${PROJECT_SOURCE_DIR}/arch/riscv/rv64imac/include"
  POSTURE KICKOS_NUM_CORES=2 KICKOS_KERNEL_CORES=2 KICKOS_MULTICORE_MODEL_SHARED=1
          KICKOS_HAVE_ASPACE=1 KICKOS_RV64_SYSOPS_FROM_SEAM=1 KICKOS_RV64_SATP_MODE=8
          KICKOS_ENABLE_SELFTEST=1)
kickos_add_unit_test(NAME map_fence SOURCES map_fence.cc
  LIBRARIES kickos_aspace_rv64imac_host)
