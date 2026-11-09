/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: one integer constant, read by C++ and by a chip's linker script.
 */
#ifndef KICKOS_ARCH_CORE_STACK_LD_H
#define KICKOS_ARCH_CORE_STACK_LD_H

/* Each core's boot stack, carved below _estack by the chip's linker script: the one the primary
 * boots on, and the one a released core parks on and takes its doorbell interrupts on. A plain
 * integer, which check_trap_redzone.sh reads as an immediate.
 */
#define KICKOS_LX6_CORE_STACK 8192

#endif
