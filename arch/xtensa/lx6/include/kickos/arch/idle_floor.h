/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * What idle's stack must hold above its thread-local carve. Included by the linker scripts'
 * cpp pass, so plain #defines only.
 */
#ifndef KICKOS_ARCH_IDLE_FLOOR_H
#define KICKOS_ARCH_IDLE_FLOOR_H

#include <kickos/arch/lx6_trap_stack.h>

/* A level-1 interrupt builds its frame and runs its dispatch below idle's own frames. */
#define KICKOS_ARCH_IDLE_FLOOR \
    (KICKOS_LX6_TRAP_FRAME + KICKOS_LX6_TRAP_DEPTH + KICKOS_LX6_TRAP_DEPTH_IDLE)

#endif
