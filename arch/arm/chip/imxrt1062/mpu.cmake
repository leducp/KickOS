# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# MPU-enforcement opt-in. This chip's linker script carves the KICKOS_HAVE_MPU
# .appdata window (so arch_domain_static_regions grants a real app-data region
# instead of reading the WEAK __kickos_appdata_* symbols as 0) AND its arch ships a
# real arch_mpu_apply -- so KICKOS_HAVE_MPU=1 actually faults a cross-domain access.
# The top CMakeLists fail-loud floor includes this in its own scope, so a plain set
# (no PARENT_SCOPE) is what it reads.
#
# The i.MX RT1062 is a Cortex-M7 (ARMv7-M) and shares the ARM PMSAv7 register map
# with the M4 boards, so it inherits the shared armv7m arch_mpu_apply unchanged --
# no chip override. SILICON-PENDING: enforcement here is BUILD-ONLY, never yet run
# on hardware (no first mpu-off boot, no flasher); this file is the enforcement
# scaffolding, not a validation claim.
set(KICKOS_CHIP_ENFORCES_MPU ON)
