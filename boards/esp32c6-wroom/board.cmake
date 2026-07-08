# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: the single source of truth for this board's arch/chip/CPU.
# Included by the board resolver (cmake/kickos.cmake) and by the RISC-V cross
# toolchain file (pre-project, for the ISA/ABI baseline). Side-effect free.
#
# ESP32-C6-WROOM-1 (ESP-RISC-V "HP CPU", RV32IMAC, M/U + PMP). Shares the rv32imac
# arch with the qemu-riscv (virt) board; the esp32c6 chip layer supplies the real
# UART/SYSTIMER/watchdog/CLINT edges. Build-only this milestone: HW flash/run is a
# short follow-up pass (the K64F two-phase pattern).
set(KICKOS_BOARD_ID    "esp32c6-wroom")
set(KICKOS_ARCH_FAMILY "riscv")
set(KICKOS_ARCH        "rv32imac")
set(KICKOS_CHIP        "esp32c6")

# RV32IMAC + ILP32, Zicsr explicit (the trap/switch path needs CSR opcodes; modern
# binutils split them out of the base ISA). Maps to the rv32imac/ilp32 multilib.
set(KICKOS_MCPU -march=rv32imac_zicsr -mabi=ilp32)
