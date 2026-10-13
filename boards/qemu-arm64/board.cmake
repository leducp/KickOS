# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU `virt` for AArch64 (Cortex-A53, EL1 bare metal), PL011 console. Run with
# `qemu-system-aarch64 -M virt -cpu cortex-a53`.
set(KICKOS_BOARD_ID    "qemu-arm64")
set(KICKOS_ARCH_FAMILY "arm64")
set(KICKOS_ARCH        "armv8a")
set(KICKOS_CHIP        "virt_arm64")
