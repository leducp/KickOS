# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
#
# QEMU `virt` for RV64. NS16550A UART console; run with
# `qemu-system-riscv64 -M virt -bios none`.
set(KICKOS_BOARD_ID    "qemu-riscv64")
set(KICKOS_ARCH_FAMILY "riscv")
set(KICKOS_ARCH        "rv64imac")
set(KICKOS_CHIP        "virt_rv64")
