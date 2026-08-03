// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Panic/fault reporting on a PUBLISHED console, in its own binary because it ends the
// system. One source, two images selected by KICKOS_PUBPANIC_CASE, since a run observes
// only one terminal event:
//   1  kos_panic -> kickos::kpanic: the "KERNEL PANIC" banner must reach the wire
//   2  an illegal instruction -> the arch fault reporter: the dump must reach the wire
//
// Both cases enter kpanic_enter with the console USER_OWNED and the buffered TX ring
// already disarmed by console_tx_deinit, so the banner can only come out over the
// RECLAIMED polled route (kernel/init/console.cc).
//
// The kos_print below is the anti-vacuity witness: console_emit DROPS it while the
// console is USER_OWNED, so the gate asserting its absence is what proves the handover
// really happened and the banner did not come out of a kernel-owned console.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/emit.h>

#ifndef KICKOS_PUBPANIC_CASE
#error "KICKOS_PUBPANIC_CASE must be 1 or 2"
#endif

using kickos::emit;

int main(int, char**)
{
    kos_print("[pubpanic] kernel-console witness (must NOT reach the wire)\n");
    // kos_send blocks on the console rendezvous, so the driver thread has run and
    // drained before the terminal event below.
    emit("[pubpanic] published route live\n");
#if KICKOS_PUBPANIC_CASE == 1
    kos_panic("[pubpanic] banner after handover");
#else
    __builtin_trap(); // x86 ud2 -> SIGILL -> the sim fault reporter
#endif
    emit("[pubpanic] ERROR: the terminal path returned\n");
    return 1;
}
