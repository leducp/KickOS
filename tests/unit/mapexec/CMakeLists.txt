# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# Build the ARM64 map editor on the host with recorded system operations.
# Use two cores to exercise peer invalidation and instruction synchronization.
set(_mapexec_backend "${PROJECT_SOURCE_DIR}/arch/arm64/armv8a/aspace_armv8a.cc")
if(NOT EXISTS "${_mapexec_backend}")
  message(FATAL_ERROR
    "KickOS: ${_mapexec_backend} is gone, so map_exec would witness nothing. This gate compiles "
    "the arm64 backend's own translation unit; a moved file breaks it silently.")
endif()

kickos_add_kernel_host_lib(kickos_aspace_armv8a_host
  SOURCES "${_mapexec_backend}" aspace_sysops_seam.cc
  INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}"
  POSTURE KICKOS_NUM_CORES=2 KICKOS_KERNEL_CORES=2 KICKOS_MULTICORE_MODEL_SHARED=1
          KICKOS_HAVE_ASPACE=1 KICKOS_ARMV8A_SYSOPS_FROM_SEAM=1)
kickos_add_unit_test(NAME map_exec SOURCES map_exec.cc
  LIBRARIES kickos_aspace_armv8a_host)

# Build a one-core variant to check local maintenance and store publication.
kickos_add_kernel_host_lib(kickos_aspace_armv8a_host_one
  SOURCES "${_mapexec_backend}" aspace_sysops_seam.cc
  INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}"
  POSTURE KICKOS_NUM_CORES=1 KICKOS_KERNEL_CORES=1 KICKOS_MULTICORE_MODEL_SHARED=1
          KICKOS_HAVE_ASPACE=1 KICKOS_ARMV8A_SYSOPS_FROM_SEAM=1)
kickos_add_unit_test(NAME map_publish SOURCES map_publish.cc
  LIBRARIES kickos_aspace_armv8a_host_one)
