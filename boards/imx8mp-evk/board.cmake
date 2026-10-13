# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# NXP i.MX 8M Plus EVK: the quad Cortex-A53 die with a GIC-500, UART1 console. Run with
# `qemu-system-aarch64 -M imx8mp-evk`, which hands over at EL3.
set(KICKOS_BOARD_ID    "imx8mp-evk")
set(KICKOS_ARCH_FAMILY "arm64")
set(KICKOS_ARCH        "armv8a")
set(KICKOS_CHIP        "imx8mp")
