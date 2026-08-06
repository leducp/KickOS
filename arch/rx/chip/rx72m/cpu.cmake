# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Renesas RX72M: RXv3 with the double-precision FPU. Both flags exist only in
# the registration-gated Renesas GNURX build.
#
# Included by the cross toolchain file pre-project(), after the board descriptor, so a
# board that genuinely differs states its own value and this file only fills what it
# left unset. Sibling of caps.cmake and mpu.cmake: a chip states its own facts.

if(NOT DEFINED KICKOS_MCPU)
  set(KICKOS_MCPU -misa=v3 -mdfpu)
endif()
