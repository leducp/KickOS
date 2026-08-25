# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU virt: RV32IMAC, soft float.
#
# Included by the cross toolchain file pre-project(), after the board descriptor.

set(KICKOS_MCPU -march=rv32imac_zicsr -mabi=ilp32)
