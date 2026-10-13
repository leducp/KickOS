# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU virt (RV64): the rv64imac multilib, soft float.
#
# The extensions are spelled out because binutils 2.4x splits rv64imac: zmmul is the multiply
# half of M, zaamo and zalrsc the two halves of A, zca the integer half of C, zicsr the
# csrr/csrw/sret the trap and switch paths need.
#
# medany: startup.S's pre-translation code reaches its own symbols PC-relative at the
# physical alias the image is loaded at, while every VMA is high.
set(KICKOS_MCPU -march=rv64imac_zicsr_zmmul_zaamo_zalrsc_zca -mabi=lp64 -mcmodel=medany)
