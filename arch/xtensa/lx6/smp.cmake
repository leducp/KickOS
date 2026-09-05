# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# What Xtensa LX6 gives every part it defines, of the six properties one kernel image across
# cores requires. The other three are the PART's and live in
# arch/xtensa/chip/<chip>/smp.cmake.
#
#   coherency   the ISA defines no coherency protocol and no maintenance operation for a memory
#               no cache sits on, so shared kernel state on such a memory is coherent by
#               absence. A CHIP OWES THAT PLACEMENT AS A LINK-TIME REFUSAL: a chip that lets
#               kernel state land behind a cache breaks this declaration silently. Ordering is
#               the Multiprocessor Synchronization Option's L32AI/S32RI (ISA summary 4.3.12,
#               p.115), a prerequisite of the Conditional Store Option below (4.3.13, p.118).
#   exclusion   S32C1I against SCOMPARE1, Special Register 12 (Conditional Store Option, ISA
#               summary 4.3.13), which arch_kernel_lock takes the word with. It plays the role
#               of both acquire and release on its own (4.3.13.5, p.122), so the claim needs no
#               barrier and the release does. ATOMCTL, SR 99, DECIDES WHETHER IT REACHES THE BUS
#               AT ALL: 4.3.13.4 and Table 52 (p.121) give per-region fields selecting
#               exception, an RCW bus transaction, or an operation local to the core, and the
#               local arm excludes a core from itself and from nobody else.
#   identity    PRID, Special Register 235 (Processor ID Option, ISA summary 6.4, p.255). The
#               ISA loads it from pins and says the value is TYPICALLY 0..NPROCESSORS-1 and need
#               not be, so a dense core index is a per-part mapping, and that mapping refuses a
#               value it does not know.
set(KICKOS_ARCH_SMP_COHERENT 1)
set(KICKOS_ARCH_SMP_EXCLUSION 1)
set(KICKOS_ARCH_SMP_IDENTITY 1)
