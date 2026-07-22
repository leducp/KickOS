# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# MPU-enforcement opt-in for the RP2350 (Cortex-M33 / PMSAv8). This chip's linker
# script carves the KICKOS_HAVE_MPU .appdata window (so arch_domain_static_regions
# grants a real app-data region instead of reading the WEAK __kickos_appdata_*
# symbols as 0). Unlike the M4/M7 boards, the M33 implements PMSAv8 (not the v7-M
# PMSA), so it CANNOT use the shared armv7m arch_mpu_apply/commit -- it pulls the
# dedicated PMSAv8 backend below, whose strong kickos_arch_mpu_commit +
# arch_mpu_region_encodable override the weak v7-M ones. See
# docs/design-rp2350-mpu-armv8m.md.
#
# The top CMakeLists fail-loud floor includes this file in its own scope, so a plain
# set (no PARENT_SCOPE) is what it reads. KICKOS_ARM_PMSAV8_SOURCE is likewise read
# by arch/CMakeLists.txt (inherited into the add_subdirectory child scope) and, when
# KICKOS_HAVE_MPU is on, appended to the chip library sources.
#
# SILICON-PENDING: the Waveshare RP2350 Pi-Zero has never booted; enforcement here is
# BUILD-ONLY, validated on hardware by the operator (mpu_fault -> clean MemManage).
set(KICKOS_CHIP_ENFORCES_MPU ON)
set(KICKOS_ARM_PMSAV8_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../../common/arch_arm_pmsav8.cc")
