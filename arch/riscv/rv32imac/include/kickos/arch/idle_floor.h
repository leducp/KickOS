/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * What idle's stack must hold above its thread-local carve. Included by the linker scripts'
 * cpp pass, so plain #defines only.
 */
#ifndef KICKOS_ARCH_IDLE_FLOOR_H
#define KICKOS_ARCH_IDLE_FLOOR_H

#include <kickos/arch/rv_trap_stack.h>

/* The msip switch frame lands below idle's own frame; every other M-mode trap idle takes moves
 * to the per-hart trap stack. */
#define KICKOS_ARCH_IDLE_FLOOR \
    (KICKOS_RV_TRAP_FRAME + KICKOS_RV_TRAP_SWITCH_DEPTH + KICKOS_RV_TRAP_DEPTH_IDLE)

#endif
