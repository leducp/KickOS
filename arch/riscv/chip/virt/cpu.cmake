# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU virt: RV32IMAC, soft float.
#
# Included by the cross toolchain file pre-project(), after the board descriptor, so a
# board that genuinely differs states its own value and this file only fills what it
# left unset. Sibling of caps.cmake and mpu.cmake: a chip states its own facts.

if(NOT DEFINED KICKOS_MCPU)
  set(KICKOS_MCPU -march=rv32imac_zicsr -mabi=ilp32)
endif()
