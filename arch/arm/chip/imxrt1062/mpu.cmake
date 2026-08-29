# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# MPU-enforcement opt-in: this chip's linker script carves the KICKOS_HAVE_MPU .appdata
# window, so arch_domain_static_regions grants a real app-data region instead of reading the
# WEAK __kickos_appdata_* symbols as 0, and its arch ships a real arch_mpu_apply.
#
# The i.MX RT1062 is a Cortex-M7 and shares the ARM PMSAv7 register map with the M4 boards,
# so it takes the shared armv7m arch_mpu_apply with no chip override. It needs one thing the
# M4s do not: the chip fixed-MPU table (kickos_arm_mpu_fixed, chip_imxrt1062.cc) wraps the
# unbacked FlexSPI/SEMC apertures as Device + XN + no-access before the I-cache comes up,
# this being the only core here that speculates (NXP ERR011573,
# docs/design-teensy-mpu-hang.md).
# Validation status of this port: see docs/reference/boards.md.
set(KICKOS_CHIP_ENFORCES_MPU ON)
