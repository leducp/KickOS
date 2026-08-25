# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# MPU-enforcement opt-in. This chip's linker script carves the KICKOS_HAVE_MPU
# .appdata window (so arch_domain_static_regions grants a real app-data region
# instead of reading the WEAK __kickos_appdata_* symbols as 0) AND its arch ships a
# real arch_mpu_apply, so KICKOS_HAVE_MPU=1 actually faults a cross-domain access.
# The top CMakeLists fail-loud floor includes this in its own scope, so a plain set
# (no PARENT_SCOPE) is what it reads; a chip that carves the window but ships no
# mpu.cmake is rejected there, so capability is an explicit opt-in, never inferred.
# Validation status of this port: see docs/reference/boards.md.
set(KICKOS_CHIP_ENFORCES_MPU ON)
