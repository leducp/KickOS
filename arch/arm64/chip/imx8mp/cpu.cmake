# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# i.MX 8M Plus: a quad Cortex-A53 cluster, and a Cortex-M7 companion this port does not build
# for.
#
# Included by the cross toolchain file pre-project(), after the board descriptor.

set(KICKOS_MCPU -mcpu=cortex-a53)
