# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# MPU-enforcement opt-in: this chip's linker script carves the KICKOS_HAVE_MPU .appdata
# window, so arch_domain_static_regions grants a real app-data region instead of reading the
# WEAK __kickos_appdata_* symbols as 0, and its arch ships a real arch_mpu_apply.
# Validation status of this port: see docs/reference/boards.md.
set(KICKOS_CHIP_ENFORCES_MPU ON)

# The MPU here is the crossbar SYSMPU, not the core PMSA: three descriptor words per region,
# encoded by chip_mk64f.cc.
set(KICKOS_ARM_MPU_BACKEND SYSMPU)
