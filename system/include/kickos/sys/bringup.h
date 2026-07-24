// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The per-board console bring-up descriptor. The default init runs it BEFORE the
// app's main, so a plain app prints through a userspace console driver with zero
// app code. Lives in the kickos_system library; keep it dependency-free (shared
// verbatim by the init body and every per-board provider TU).
//
// HARD RULE: NO libc stdio anywhere in the bring-up choreography. Between the
// publish and the driver's first recv, the publisher holds the only WAIT cap, so a
// stray printf (which routes through _write to the console endpoint) self-deadlocks
// in the rendezvous. Diagnostics in the choreography use kos::print (the RTT /
// kernel debug path), never stdio.

#ifndef KICKOS_SYS_BRINGUP_H
#define KICKOS_SYS_BRINGUP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// One board's console bring-up hook. `start` is the driver's one-shot handover
// choreography (create the console endpoint, publish it, spawn the unprivileged
// driver, drop the parent's WAIT cap); it returns 0 on success or a negative code
// on failure. `driver_prio` is the priority passed to `start`. A board with no
// userspace console driver sets start = NULL: the default init then keeps the
// kernel console (the universal default, not an error).
struct kos_console_bringup
{
    int (*start)(uint8_t driver_prio);
    uint8_t driver_prio;
};

// The selected board's console descriptor. EXACTLY ONE definition links per image,
// chosen by the KICKOS_CONSOLE_BRINGUP CMake target (default kickos_console_none,
// start = NULL). See system/init/console_none.cc and a per-driver provider such as
// user/driver/xmcuart/console_bringup.cc.
extern struct kos_console_bringup const kickos_board_console;

#ifdef __cplusplus
}
#endif

#endif
