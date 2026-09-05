/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Ends the kernel lock's span over a context swap. ASSEMBLY ONLY: this header defines one
 * macro and declares nothing a C translation unit can use.
 *
 * Single-sourced: the cooperative swap ends in arch/xtensa/lx6/switch.S and the deferred
 * preemptive one ends in the chip's level-1 interrupt entry.
 *
 * WHERE IT MAY BE CALLED, every clause load-bearing on this ISA:
 *
 *   AFTER SPILL_ALL_WINDOWS. xtensa_switch writes from->sp/ps/pc before it spills, so between
 *   those two the outgoing context is published while its ancestor frames still live only in
 *   this core's physical AR file, and a peer that took `from` there would retw into a base save
 *   area nothing has written yet.
 *
 *   ONCE a1 NAMES THE INCOMING FRAME, so the call frame lands on the incoming thread's own
 *   stack below its saved frame and the resume tail reloads everything the call may clobber.
 *
 *   BEFORE THE a0 LOAD on a retw tail. a0 carries the incoming return address with its CALLINC
 *   field and any call clobbers it. a3 survives: the caller's a0-a3 sit below the callee window
 *   of a call4.
 *
 * A window overflow spills a0-a3 to [a1-16, a1), which on a retw tail is exactly the incoming
 * thread's base save area. The window file is a single live frame at every call site below, so
 * one call4 into a shallow leaf cannot wrap the 64-AR file and cannot overflow. That is a
 * property of the CALL SITES: do not call this where more than one window is live.
 */

#ifndef KICKOS_ARCH_LX6_SWITCH_UNLOCK_H
#define KICKOS_ARCH_LX6_SWITCH_UNLOCK_H

    .macro  SWITCH_UNLOCK
#if KICKOS_KERNEL_CORES > 1
    call4   kickos_switch_unlock
#endif
    .endm

#endif /* KICKOS_ARCH_LX6_SWITCH_UNLOCK_H */
