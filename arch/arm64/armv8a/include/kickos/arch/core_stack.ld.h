/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: one integer constant, read by C++ and by a chip's linker script.
 */
#ifndef KICKOS_ARCH_CORE_STACK_LD_H
#define KICKOS_ARCH_CORE_STACK_LD_H

/* Every core's boot stack: the primary's, which the chip's linker script reserves below _estack,
 * and each secondary's g_core_stack row. A plain integer, which check_trap_redzone.sh reads as an
 * immediate.
 */
#define KICKOS_ARMV8A_CORE_STACK 65536

#endif
