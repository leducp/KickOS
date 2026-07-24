// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The default kickos_init_entry provider. A DISTINCT TU from the run body so a
// delegating override that references kickos_default_init_run does not also drag in
// this default kickos_init_entry.

#include <kickos/sys/init.h>

extern "C" int kickos_init_entry(int argc, char** argv)
{
    // Order is clock->pinmux->service list->console hook->app. Apply the board pin
    // map first (the DAG middle): a nonzero result is a loud failure, returned WITHOUT
    // running the app.
    int const pinmux_rc = kickos_pinmux_run();
    if (pinmux_rc != 0)
    {
        return pinmux_rc;
    }
    // Bring the board's service list up. During the M4.4 migration this runs
    // ALONGSIDE the console hook below: a migrated board (frdmk64f, xmc4800-relax)
    // fills its list -- console included as a KOS_SVC_CONSOLE entry -- and sets its
    // console hook to none; an un-migrated board has an empty list (no-op) and keeps
    // its hook. So exactly one of the two runners does work per board: never a double
    // bring-up, never a missing console. A nonzero result is a loud failure: return it
    // WITHOUT running the app.
    int const svc_rc = kickos_service_list_run();
    if (svc_rc != 0)
    {
        return svc_rc;
    }
    // Bring the console up next. On a board with a userspace console driver this
    // performs the handover, so the app's stdout reaches the wire through the driver
    // with zero app code; on a board with none it is a no-op (kernel console kept).
    // A nonzero result is a loud failure: return it WITHOUT running the app, so the
    // app never runs against a dark console.
    int const rc = kickos_console_bringup_run();
    if (rc != 0)
    {
        return rc;
    }
    return kickos_default_init_run(argc, argv);
}
