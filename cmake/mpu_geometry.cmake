# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The region descriptors one thread's region set carries in its TCB, root's included. Structural,
# not configuration: no Kconfig symbol, no defconfig states one, the ARMv6-M and ARMv7-M units
# having eight. Declared in the build, which exports root's free regions in the manifest's `init`
# section from it, and emitted to C through the generated <kickos/config/mpu_geometry.h>.
set(KICKOS_MPU_MAX_REGIONS 8)
kickos_emit_geometry(kickos/config/mpu_geometry.h KICKOS_MPU_MAX_REGIONS)
