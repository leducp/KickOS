// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F console bring-up provider: point the default init at the k64uart handover
// choreography (k64uart_console_start). Selected by KICKOS_CONSOLE_BRINGUP on
// frdmk64f enforcement builds. driver_prio 12 matches the demo's DRIVER_PRIO
// and must be >= every stdout client's priority (D9: no PI on rendezvous).

#include <kickos/sys/bringup.h>

#include <kickos/driver/k64uart.h>

extern "C"
{
    struct kos_console_bringup const kickos_board_console = { k64uart_console_start, 12 };
}
