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
# AT TWO KERNEL CORES, which is why this gate builds its own object library rather than joining
# any of the K-seam ones: removal_owes_rendezvous, peer_cores and instruction_side_rendezvous are
# each split on KICKOS_KERNEL_CORES > 1 with a no-op #else, so at the preset's one core the
# predicate is `return false`, the peer set is 0, and every arm below passes vacuously. The
# definitions are PUBLIC so the arms' own translation unit takes them too, where a static_assert
# holds the count.
#
# The core counts AND THE MODEL reach C through the root's add_compile_definitions, which this
# directory INHERITS, so each is dropped from the inherited copy rather than shadowed: two -D of
# one macro with different values is a redefinition -Werror refuses. TWO PLACES CARRY IT: this
# directory inherited the root's copy, and kickos_kernel's own COMPILE_DEFINITIONS was seeded
# from the same place when that target was created.
get_directory_property(_mapexec_dirdefs COMPILE_DEFINITIONS)
list(FILTER _mapexec_dirdefs EXCLUDE
     REGEX "^KICKOS_((NUM|KERNEL)_CORES|MULTICORE_MODEL_SHARED|HAVE_ASPACE)=")
set_directory_properties(PROPERTIES COMPILE_DEFINITIONS "${_mapexec_dirdefs}")

set(_mapexec_backend "${PROJECT_SOURCE_DIR}/arch/arm64/armv8a/aspace_armv8a.cc")
if(NOT EXISTS "${_mapexec_backend}")
  message(FATAL_ERROR
    "KickOS: ${_mapexec_backend} is gone, so map_exec would witness nothing. This gate compiles "
    "the arm64 backend's own translation unit; a moved file breaks it silently.")
endif()

add_library(kickos_aspace_armv8a_host OBJECT
  "${_mapexec_backend}"
  aspace_sysops_seam.cc)

target_compile_features(kickos_aspace_armv8a_host PUBLIC ${KICKOS_CXX_STANDARD})
target_include_directories(kickos_aspace_armv8a_host PUBLIC
  $<TARGET_PROPERTY:kickos_kernel,INCLUDE_DIRECTORIES>
  "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(kickos_aspace_armv8a_host PUBLIC
  "$<FILTER:$<TARGET_PROPERTY:kickos_kernel,COMPILE_DEFINITIONS>,EXCLUDE,^KICKOS_((NUM|KERNEL)_CORES|MULTICORE_MODEL_SHARED|HAVE_ASPACE)=>"
  KICKOS_NUM_CORES=2
  KICKOS_KERNEL_CORES=2
  KICKOS_MULTICORE_MODEL_SHARED=1
  KICKOS_HAVE_ASPACE=1)
target_compile_options(kickos_aspace_armv8a_host PRIVATE ${KICKOS_WARN_FLAGS}
  -fno-exceptions -fno-rtti -fno-threadsafe-statics)

add_executable(map_exec map_exec.cc)
target_link_libraries(map_exec PRIVATE kickos_aspace_armv8a_host GTest::gtest_main)
target_compile_options(map_exec PRIVATE ${KICKOS_WARN_FLAGS})
kickos_discover_unit_tests(map_exec)
