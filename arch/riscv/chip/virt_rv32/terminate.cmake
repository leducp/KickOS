# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Exiting-dead-end opt-in: this chip's arch_shutdown is a RISC-V semihosting
# SYS_EXIT_EXTENDED, which ends the emulator with a status the harness reads, so a fault
# exits instead of blinking a LED this virtual board does not have.
# Validation status of this port: see docs/reference/boards.md.
set(KICKOS_CHIP_EXITS_ON_FAULT ON)
