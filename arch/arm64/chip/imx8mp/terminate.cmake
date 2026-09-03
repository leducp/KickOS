# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Exiting-dead-end opt-in: this chip's arch_shutdown is an AArch64 semihosting SYS_EXIT,
# which ends the emulator with a status the harness reads, so a fault exits instead of
# spinning where no gate can tell it from a slow boot.
set(KICKOS_CHIP_EXITS_ON_FAULT ON)
