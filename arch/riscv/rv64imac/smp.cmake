# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# rv64imac meets every property a part running ONE kernel image across cores requires, and by
# what mechanism:
#
#   coherency       RVWMO is a single coherent memory over every hart, and this port programs no
#                   cacheable/non-cacheable distinction: there is no maintenance operation in
#                   the ISA for shared kernel state and none is owed.
#   exclusion       LR/SC over the A extension (Zalrsc in this board's baseline), which
#                   arch_kernel_lock takes the word with.
#   inter-core IRQ  a CLINT msip word per hart. It raises a MACHINE software interrupt, which
#                   mideleg holds read-only zero, so the chip's machine-mode trampoline lowers
#                   it to mip.SSIP (arch/riscv/chip/virt_rv64/startup.S).
#   identity        a dense index published in sscratch at bring-up and read by arch_cpu_id
#                   (arch/riscv/rv64imac/percpu.h). NOT mhartid, which is integrator-chosen.
#   symmetry        one set of identical harts, so a thread runs the same on any of them.
#   targeting       DECLARED BY THE CHIP AND NOT BY THIS FILE. The `virt` machine wires a PLIC,
#                   whose enable bits are per hart context, so a device line is pinned rather
#                   than masked. This port drives no PLIC line today: its interrupt controller
#                   is the software one in arch_rv64imac.cc, whose lines are per hart already.
set(KICKOS_ARCH_SMP_CAPABLE 1)
