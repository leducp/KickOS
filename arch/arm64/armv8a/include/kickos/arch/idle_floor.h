/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * What idle's stack must hold above its thread-local carve. Included by the linker scripts'
 * cpp pass, so plain #defines only.
 */
#ifndef KICKOS_ARCH_IDLE_FLOOR_H
#define KICKOS_ARCH_IDLE_FLOOR_H

#include <kickos/arch/armv8a_trap_stack.h>

/* An interrupt taken at EL1h builds its frame and runs its dispatch below idle's own frame. */
#define KICKOS_ARCH_IDLE_FLOOR (KICKOS_ARMV8A_TRAP_NEST + KICKOS_ARMV8A_TRAP_DEPTH_IDLE)

#endif
