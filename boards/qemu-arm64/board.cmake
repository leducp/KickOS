# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: which arch and which chip, and any CPU flag that is this BOARD's
# rather than its chip's. Included by the board resolver (cmake/kickos.cmake) and by the
# cross toolchain file pre-project(), which then includes the chip's own cpu.cmake for
# the flags left unset here. Side-effect free: set only these.
#
# QEMU `virt` for AArch64 (Cortex-A53, EL1 bare metal, 64-bit) is the runnable armv8a
# verification target, the AArch64 analog of the `qemu-riscv` board. PL011 UART console;
# run with `qemu-system-aarch64 -M virt -cpu cortex-a53`.
set(KICKOS_BOARD_ID    "qemu-arm64")
set(KICKOS_ARCH_FAMILY "arm64")
set(KICKOS_ARCH        "armv8a")
set(KICKOS_CHIP        "virt_arm64")
