# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Atmel SAM3X8E: Cortex-M3, no FPU.
#
# Included by the cross toolchain file pre-project(), after the board descriptor, so a
# board that genuinely differs states its own value and this file only fills what it
# left unset. Sibling of caps.cmake and mpu.cmake: a chip states its own facts.

if(NOT DEFINED KICKOS_MCPU)
  set(KICKOS_MCPU -mcpu=cortex-m3)
endif()

if(NOT DEFINED KICKOS_MFLOAT_ABI)
  set(KICKOS_MFLOAT_ABI soft)
endif()
