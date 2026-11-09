/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * FLUSH_WINDOWS: store every live window but the current one to its stack, leaving a single
 * WindowStart bit set.
 *
 * Needs PS.WOE=1, PS.EXCM=0 and INTLEVEL >= EXCM_LEVEL, and a1 must be the current frame's
 * real SP: the nearest ancestor's a0-a3 land just below it.
 *
 * Sized for a 64-AR file. A reference to a12 overflows any live window among the next three
 * quads (WindowCheck, Xtensa ISA Summary 6.1.3), so five such references three quads apart
 * cover the other fifteen, and the ROTW steps (8.3.268) sum to 16 to bring WindowBase home.
 */

#ifndef KICKOS_ARCH_LX6_FLUSH_WINDOWS_INC
#define KICKOS_ARCH_LX6_FLUSH_WINDOWS_INC

    .macro  FLUSH_WINDOWS
    and     a12, a12, a12
    rotw    3
    and     a12, a12, a12
    rotw    3
    and     a12, a12, a12
    rotw    3
    and     a12, a12, a12
    rotw    3
    and     a12, a12, a12
    rotw    4
    .endm

#endif
