// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A console driver that prints at its start, through both of its instances. The image is built
// with KICKOS_SIMCON_EXIT_AFTER=2, so each instance serves two lines, then exits; the init
// restarts it once.
//
// Main runs above the driver while the first instance serves, so its third line is parked on the
// console when the driver exits. The init can still restart the driver, so the line waits and the
// restarted instance, which serves the same endpoint, takes it first. Once that instance has
// ended too, the init is out of restarts and drops the endpoint, so a line is refused. Every
// verdict is main's exit status.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/emit.h>
#include <kickos/sys/errno.h>

#include <stdlib.h>
#include <string.h>

namespace
{
    // Above the driver's 12, at main's ceiling.
    constexpr uint8_t ABOVE_DRIVER = 13;
    constexpr uint8_t BELOW_INIT = 2;

    kickos::WriteResult write_line(char const* s)
    {
        return kickos::stdout_write(s, strlen(s));
    }

    // One death of the driver, as the init reports it on bit 0.
    bool death_seen()
    {
        uint32_t bits = 0;
        return kos_notify_wait(1u, KOS_NEVER, &bits) == 0 and (bits & 1u) != 0;
    }
}

int main(int, char**)
{
    kos_cap_t const events = kos_grant_notify("/init/events");
    if (kos_notify_bind(events) != 0)
    {
        exit(2);
    }
    if (kos_thread_set_priority(ABOVE_DRIVER) != 0)
    {
        exit(4);
    }
    kos::print("[conrestart] served by the first instance\n");
    kos::print("[conrestart] served by the first instance, its last line\n");
    kickos::WriteResult const parked =
        write_line("[conrestart] parked across the driver's death, served by the restart\n");
    // Still above the driver, so the first death's bit is taken before the restarted instance can
    // end and raise it again.
    if (not death_seen())
    {
        exit(3);
    }
    if (kos_thread_set_priority(BELOW_INIT) != 0)
    {
        exit(4);
    }
    if (parked.error != 0)
    {
        exit(5);
    }
    if (not death_seen())
    {
        exit(3);
    }
    kickos::WriteResult const refused =
        write_line("[conrestart] past the last instance (must NOT reach the wire)\n");
    if (refused.sent != 0u or refused.error != -KOS_ECONNREFUSED)
    {
        exit(6);
    }
    exit(0);
}
