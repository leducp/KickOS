# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Nordic nRF51822: Cortex-M0, no FPU.
#
# Included by the cross toolchain file pre-project(), after the board descriptor.

set(KICKOS_MCPU -mcpu=cortex-m0)

if(NOT DEFINED KICKOS_MFLOAT_ABI)
  set(KICKOS_MFLOAT_ABI soft)
endif()
