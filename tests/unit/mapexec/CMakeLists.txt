# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# A map whose break-before-make replaces an EXECUTABLE leaf owes the instruction-side
# rendezvous, and what this gate reads is the WIRING: whether map_into consults the predicate at
# all, whether the debt survives its recursion, whether arch_aspace_map rings with the peer set
# that stood before its edits, and whether a failed map's rollback pays the same debt for the
# leaves it removes.
#
# THE ARM64 BACKEND'S OWN TRANSLATION UNIT, compiled for the host. Its system operations are
# declared rather than emitted off AArch64 (arch/arm64/armv8a/sysops_armv8a.h), and
# aspace_sysops_seam.cc answers those plus the frame pool, the physical window and the
# rendezvous itself. The private header is reached through the source directory, which the
# quoted include in the backend resolves without help.
#
# AT TWO KERNEL CORES: removal_owes_rendezvous, peer_cores and instruction_side_rendezvous are
# each split on KICKOS_KERNEL_CORES > 1 with a no-op #else, so at the preset's one core the
# predicate is `return false`, the peer set is 0, and every arm below passes vacuously. The
# posture is PUBLIC on the library, so the backend's own TU takes it too, where a
# static_assert holds the count.
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
          KICKOS_HAVE_ASPACE=1)
kickos_add_unit_test(NAME map_exec SOURCES map_exec.cc
  LIBRARIES kickos_aspace_armv8a_host)
