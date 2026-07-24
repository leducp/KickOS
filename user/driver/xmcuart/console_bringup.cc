// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 console bring-up provider: point the default init at the xmcuart handover
// choreography (xmcuart_console_start). Selected by KICKOS_CONSOLE_BRINGUP on
// xmc4800-relax enforcement builds. driver_prio 12 matches the demo's DRIVER_PRIO
// and must be >= every stdout client's priority (D9: no PI on rendezvous).

#include <kickos/sys/bringup.h>

#include <kickos/driver/xmcuart.h>

extern "C"
{
    struct kos_console_bringup const kickos_board_console = { xmcuart_console_start, 12 };
}
