# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The consumer behind tests/integration/check_pool_abi.sh: an image composed out of tree against an
# installed KickOS package, its kernel's pools sized by the manifest KICKOS_POOL_ABI_MANIFEST names,
# the package's by default. With KICKOS_POOL_ABI_SKEW, a directory, its pools TU reads the pool
# header found there ahead of the package's.

cmake_minimum_required(VERSION 3.24)
project(kickos_pool_abi LANGUAGES C CXX ASM)

find_package(KickOS REQUIRED)
if(KICKOS_POOL_ABI_MANIFEST)
  set(KICKOS_MANIFEST "${KICKOS_POOL_ABI_MANIFEST}")
endif()

add_executable(probe main.c)
kickos_compose(probe_system "${KICKOS_PROBE_COMPOSITION}")
target_link_libraries(probe PRIVATE KickOS::kickos probe_system)
if(KICKOS_POOL_ABI_SKEW)
  target_include_directories(probe_system_kernel BEFORE PRIVATE "${KICKOS_POOL_ABI_SKEW}")
endif()
kickos_emit_image(probe)
