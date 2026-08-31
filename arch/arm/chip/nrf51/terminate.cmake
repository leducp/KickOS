# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Exiting-dead-end opt-in: this chip's arch_shutdown is an ARM semihosting
# SYS_EXIT_EXTENDED. micro:bit is a real board WITH a diag LED, and it takes the exiting
# dead-end anyway because CI runs the same binary under QEMU, where the blink terminal
# would spin until the harness times out.
set(KICKOS_CHIP_EXITS_ON_FAULT ON)
