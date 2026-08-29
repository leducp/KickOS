# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Exiting-dead-end opt-in: this chip's arch_shutdown is an ARM semihosting
# SYS_EXIT_EXTENDED, which ends the emulator with a status the harness reads, so a fault
# exits instead of blinking a LED these FPGA images do not have.
# Validation status of these ports: see docs/reference/boards.md.
set(KICKOS_CHIP_EXITS_ON_FAULT ON)
