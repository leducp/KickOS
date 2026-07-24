// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The default init's console bring-up step: read the selected board descriptor and
// run its handover choreography before the app's main. A board with no driver
// (start = NULL) keeps the kernel console.
//
// HARD RULE (see <kickos/sys/bringup.h>): no libc stdio here. The choreography holds
// the only WAIT cap between publish and the driver's first recv, so a printf would
// self-deadlock in the rendezvous. This TU touches no console at all.

#include <kickos/sys/bringup.h>
#include <kickos/sys/init.h>

extern "C" int kickos_console_bringup_run(void)
{
    if (kickos_board_console.start == nullptr)
    {
        return 0; // no userspace driver: keep the kernel console
    }
    return kickos_board_console.start(kickos_board_console.driver_prio);
}
