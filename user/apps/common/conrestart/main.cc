// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A console driver that prints at its start, through both of its instances. The image is built
// with KICKOS_SIMCON_EXIT_AFTER=2, so each instance serves the handover probe and one line of
// main's, then exits; the init restarts it once. Sequenced by the init's death report, not by
// sleeps.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <stdlib.h>

extern "C" void conrestart_main(kos_self_t const* self)
{
    kos_cap_t const events = kos_grant_notify(self, "/init/events");
    if (kos_notify_bind(events) != 0)
    {
        kos::print("[conrestart] ERROR: no /init/events to wait the driver's death on\n");
        exit(2);
    }
    kos::print("[conrestart] served by the first instance\n");
    uint32_t bits = 0;
    if (kos_notify_wait(events, 1u, KOS_TIMEOUT_NONE, &bits) != 0 or (bits & 1u) == 0)
    {
        kos::print("[conrestart] ERROR: the init never reported the first instance's death\n");
        exit(3);
    }
    // Waits out the dark window until the restart's publish, then goes to the second instance.
    kos::print("[conrestart] served by the restarted instance\n");
    exit(0);
}
