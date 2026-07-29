// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The default kickos_init_entry provider. A DISTINCT TU from the run body so a
// delegating override that references kickos_default_init_run does not also drag in
// this default kickos_init_entry.

#include <kickos/sys/init.h>

extern "C" int kickos_init_entry(int argc, char** argv)
{
    // Order is clock->pinmux->service list->app. Apply the board pin map first (the
    // DAG middle): a nonzero result is a loud failure, returned WITHOUT running the app.
    int const pinmux_rc = kickos_pinmux_run();
    if (pinmux_rc != 0)
    {
        return pinmux_rc;
    }
    // Bring the board's service list up. On a board with a userspace console driver the
    // console is the list's first KOS_SVC_CONSOLE entry, so the app's stdout reaches the
    // wire through the driver with zero app code; a board with an empty list (no-op)
    // keeps the kernel console. A nonzero result is a loud failure: return it WITHOUT
    // running the app, so the app never runs against a half-brought-up board.
    int const svc_rc = kickos_service_list_run();
    if (svc_rc != 0)
    {
        return svc_rc;
    }
    // Runs AFTER bring-up on purpose: the run body narrows root's authority to what the
    // app declared, and the pin map and the console publish above need bits the app does
    // not get.
    return kickos_default_init_run(argc, argv);
}
