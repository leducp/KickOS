# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU `virt` for RV32IMAC, M-mode bare metal, CLINT timer and semihosting. Run with
# `qemu-system-riscv32 -M virt -bios none`.
set(KICKOS_BOARD_ID    "qemu-riscv")
set(KICKOS_ARCH_FAMILY "riscv")
set(KICKOS_ARCH        "rv32imac")
set(KICKOS_CHIP        "virt_rv32")
