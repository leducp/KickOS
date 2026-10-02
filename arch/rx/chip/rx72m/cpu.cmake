# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Renesas RX72M: RXv3 with the double-precision FPU. Both flags exist only with
# Renesas's changes to GCC, which the KickOS toolchain's rx-elf family carries.
#
# Included by the cross toolchain file pre-project(), after the board descriptor.

set(KICKOS_MCPU -misa=v3 -mdfpu)
