# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
#
# QEMU `q35` under UEFI firmware. COM1 console; run with tools/run-qemu-x86_64.sh,
# which builds the EFI system partition and boots OVMF.
#
# KICKOS_ARCH_FAMILY is spelled out: the deriver in cmake/kickos.cmake falls through to
# the arch name for anything that is not armv* or sim, which would say "x86_64".
set(KICKOS_BOARD_ID    "qemu-x86_64")
set(KICKOS_ARCH_FAMILY "x86")
set(KICKOS_ARCH        "x86_64")
set(KICKOS_CHIP        "q35")
