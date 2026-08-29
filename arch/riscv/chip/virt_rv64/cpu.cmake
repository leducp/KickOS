# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU virt (RV64): the rv64imac multilib, soft float.
#
# Included by the cross toolchain file pre-project(), after the board descriptor.
#
# The extensions are spelled out because binutils 2.4x splits what rv64imac used to imply:
# zmmul is the multiply half of M, zaamo and zalrsc the two halves of A, zca the integer
# half of C. Naming them selects the same multilib the short form used to, and so does
# adding zicsr, which the trap and switch paths need for csrr/csrw/sret.
#
# medany, not the medlow default: PC-relative, so startup.S's pre-translation code reaches its
# own symbols at the physical alias the image is loaded at while every VMA is high
# (arch/riscv/chip/virt_rv64/startup.S). The image's own placement is inside medlow's reach:
# lui+addi materialises bits 31:12 sign-extended, which is what the prebuilt libc and libgcc
# multilibs use and why 0xFFFFFFFF_80000000 is the base that links at all.
set(KICKOS_MCPU -march=rv64imac_zicsr_zmmul_zaamo_zalrsc_zca -mabi=lp64 -mcmodel=medany)
