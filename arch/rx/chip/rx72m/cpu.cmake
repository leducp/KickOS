# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Renesas RX72M: RXv3 with the double-precision FPU. Both flags exist only in
# the registration-gated Renesas GNURX build.
#
# Included by the cross toolchain file pre-project(), after the board descriptor.

set(KICKOS_MCPU -misa=v3 -mdfpu)
