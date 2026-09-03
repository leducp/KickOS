# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# What THIS PART decides, of the six properties one kernel image across cores requires. The
# other three are the ISA's and live in arch/riscv/rv64imac/smp.cmake.
#
#   inter-core IRQ  a CLINT msip word per hart. It raises a MACHINE software interrupt and
#                   mideleg holds read-only zero on this machine, so the receive side runs
#                   through this chip's machine-mode trampoline (startup.S); a supervisor
#                   store to a peer's msip word is permitted, so the send has no
#                   machine-mode leg.
#   symmetry        one set of identical harts, so a thread runs the same on any of them.
#   targeting       the machine wires a PLIC whose enable bits are per hart context, so a
#                   device line is pinned rather than masked on the harts that must not take
#                   it. THIS PORT DRIVES NO PLIC LINE, so the declaration is the machine's:
#                   its interrupt controller is the software one in arch_rv64imac.cc, whose
#                   lines are per hart already.
set(KICKOS_CHIP_SMP_INTERRUPT 1)
set(KICKOS_CHIP_SMP_SYMMETRIC 1)
set(KICKOS_CHIP_SMP_TARGETING 1)
