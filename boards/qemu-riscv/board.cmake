# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: which arch and which chip, and any CPU flag that is this BOARD's
# rather than its chip's. Included by the board resolver (cmake/kickos.cmake) and by the
# cross toolchain file pre-project(), which then includes the chip's own cpu.cmake for
# the flags left unset here. Side-effect free: set only these.
#
# QEMU `virt` (RISC-V RV32IMAC, M-mode bare metal) is the runnable rv32imac
# verification target, the RISC-V analog of the ARM `qemu`/mps2 board. Uses the
# standard CLINT (mtime/mtimecmp/msip) + RISC-V semihosting; run with
# `qemu-system-riscv32 -M virt -bios none`.
set(KICKOS_BOARD_ID    "qemu-riscv")
set(KICKOS_ARCH_FAMILY "riscv")
set(KICKOS_ARCH        "rv32imac")
set(KICKOS_CHIP        "virt")

# RV32IMAC + ILP32: integer core (no F/D -> soft-float), atomics + compressed.
# Zicsr is explicit (modern binutils split the CSR opcodes out of the base ISA;
# the switch/trap path needs csrr/csrw/mret). It maps to the same rv32imac/ilp32
# newlib/libgcc multilib. Same flags on compile and link select that multilib.
