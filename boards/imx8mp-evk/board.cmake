# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Board descriptor: which arch and which chip, and any CPU flag that is this BOARD's
# rather than its chip's. Included by the board resolver (cmake/kickos.cmake) and by the
# cross toolchain file pre-project(), which then includes the chip's own cpu.cmake for
# the flags left unset here. Side-effect free: set only these.
#
# The NXP i.MX 8M Plus EVK: a quad Cortex-A53 die with a GIC-500 and a Cortex-M7 companion
# this port does not build for. UART1 console at 0x3086_0000; run with
# `qemu-system-aarch64 -M imx8mp-evk`, which hands over at EL3.
set(KICKOS_BOARD_ID    "imx8mp-evk")
set(KICKOS_ARCH_FAMILY "arm64")
set(KICKOS_ARCH        "armv8a")
set(KICKOS_CHIP        "imx8mp")
