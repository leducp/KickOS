# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# What this ISA gives every part it defines, of the six properties one kernel image across
# cores requires. The other three are the PART's and live in
# arch/riscv/chip/<chip>/smp.cmake.
#
#   coherency   RVWMO is a single coherent memory over every hart, and this port programs no
#               cacheable/non-cacheable distinction: there is no maintenance operation in the
#               ISA for shared kernel state and none is owed.
#   exclusion   LR/SC over the A extension (Zalrsc in this port's baseline), which
#               arch_kernel_lock takes the word with.
#   identity    a dense index published in sscratch at bring-up and read by arch_cpu_id
#               (arch/riscv/rv64imac/percpu.h). NOT mhartid, which is integrator-chosen.
set(KICKOS_ARCH_SMP_COHERENT 1)
set(KICKOS_ARCH_SMP_EXCLUSION 1)
set(KICKOS_ARCH_SMP_IDENTITY 1)
