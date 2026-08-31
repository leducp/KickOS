# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Exiting-dead-end opt-in: this chip's arch_shutdown writes the QEMU `virt` SiFive test
# finisher, which stops the machine with a status the harness reads, so a fault exits
# instead of blinking a LED this virtual board does not have.
set(KICKOS_CHIP_EXITS_ON_FAULT ON)
