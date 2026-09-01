/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * NOT a C header: one integer constant, read by C++ and by a chip's linker script.
 */
#ifndef KICKOS_ARCH_CORE_STACK_LD_H
#define KICKOS_ARCH_CORE_STACK_LD_H

/* Every core's SP_EL1: the primary's, reserved by the chip's linker script, and each
 * secondary's own block. Every exception a core takes builds its frame here, the panic
 * reporter's descent included.
 */
#define KICKOS_ARMV8A_CORE_STACK (64 * 1024)

#endif
