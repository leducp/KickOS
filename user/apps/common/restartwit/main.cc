// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Waits out both of the witness's starts on the init's death reports, then ends the system.

#include <kickos/kos.h>
#include <kickos/sys.h>

int main(int, char**)
{
    kos_cap_t const events = kos_grant_notify("/init/events");
    if (kos_notify_bind(events) != 0)
    {
        kos::print("[restartwit] ERROR: no /init/events to wait the witness's deaths on\n");
        kos_exit(3);
    }
    struct kos_task_status s = {};
    while (true)
    {
        if (kos_task_status(0, &s) == 0 and not s.alive)
        {
            break;
        }
        uint32_t bits = 0;
        if (kos_notify_wait(events, 1u, KOS_TIMEOUT_NONE, &bits) != 0)
        {
            kos::print("[restartwit] ERROR: the init never reported the witness's death\n");
            kos_exit(4);
        }
    }
    if (s.deaths != 2u)
    {
        kos::print("[restartwit] FAIL the witness did not end twice\n");
        kos_exit(5);
    }
    kos::print("[restartwit] the witness ended twice\n");
    kos_exit(0);
}
