# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: which arch and which chip, and any CPU flag that is this BOARD's
# rather than its chip's. Included by the board resolver (cmake/kickos.cmake) and by the
# cross toolchain file pre-project(), which then includes the chip's own cpu.cmake for
# the flags left unset here. Side-effect free: set only these.
#
# QEMU mps2-an500 (Cortex-M7): the runnable M7 target, with 16 MPU regions where
# the M4 has 8. Shares the AN386 memory map, so the `mps2` chip linker script
# serves it unchanged; the whole board is this descriptor.
set(KICKOS_BOARD_ID "qemu-m7")
set(KICKOS_ARCH "armv7m")
set(KICKOS_CHIP "mps2")
# The one chip in the tree whose CORE is not a chip fact: arch/arm/chip/mps2 serves four
# QEMU FPGA images that declare four different cores, so it ships no cpu.cmake and each
# board states its own.
set(KICKOS_MCPU -mcpu=cortex-m7 -mfpu=fpv5-d16)
set(KICKOS_MFLOAT_ABI softfp)
