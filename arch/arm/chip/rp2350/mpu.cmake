# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# MPU-enforcement opt-in: this chip's linker script carves the KICKOS_HAVE_MPU .appdata
# window, so arch_domain_static_regions grants a real app-data region instead of reading the
# WEAK __kickos_appdata_* symbols as 0.
#
# The Cortex-M33 implements PMSAv8, not the v7-M PMSA, so it cannot take the shared armv7m
# arch_mpu_apply/commit and pulls the dedicated backend below instead, whose strong
# kickos_arch_mpu_commit and arch_mpu_region_encodable replace the v7-M fallback TUs.
set(KICKOS_CHIP_ENFORCES_MPU ON)
set(KICKOS_ARM_PMSAV8_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../../common/arch_arm_pmsav8.cc")
set(KICKOS_ARM_MPU_BACKEND PMSAV8)
