# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# What ARMv8-A gives every part it defines, of the six properties one kernel image across
# cores requires. The other three are the PART's and live in
# arch/arm64/chip/<chip>/smp.cmake.
#
#   coherency   the architecture defines the inner-shareable domain and the shareability
#               attribute that places Normal memory in it, so shared kernel state needs no
#               software maintenance. A CHIP OWES SH=0b11 ON ITS NORMAL BOOT DESCRIPTORS for
#               that to hold, which is what the boot tables in each chip's startup.S and
#               aspace_armv8a.cc program; a chip mapping kernel state non-shareable breaks
#               this declaration silently.
#   exclusion   LDXR/STXR over that same domain, architectural on ARMv8-A rather than an
#               integration option, so no hardware lock block is needed.
#   identity    MPIDR_EL1, which arch_cpu_id reads (arch_armv8a.cc).
set(KICKOS_ARCH_SMP_COHERENT 1)
set(KICKOS_ARCH_SMP_EXCLUSION 1)
set(KICKOS_ARCH_SMP_IDENTITY 1)
