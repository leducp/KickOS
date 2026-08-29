# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: which arch and which chip, and any CPU flag that is this BOARD's
# rather than its chip's. Included by the board resolver (cmake/kickos.cmake) and by the
# cross toolchain file pre-project(), which then includes the chip's own cpu.cmake for
# the flags left unset here. Side-effect free: set only these.
#
# QEMU `virt` for RV64 is the runnable rv64imac verification target, the 64-bit RISC-V
# analog of the `qemu-arm64` board. NS16550A UART console; run with
# `qemu-system-riscv64 -M virt -bios none`.
set(KICKOS_BOARD_ID    "qemu-riscv64")
set(KICKOS_ARCH_FAMILY "riscv")
set(KICKOS_ARCH        "rv64imac")
set(KICKOS_CHIP        "virt_rv64")
