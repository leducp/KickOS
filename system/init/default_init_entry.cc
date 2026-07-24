// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The default kickos_init_entry provider. A DISTINCT TU from the run body so a
// delegating override that references kickos_default_init_run does not also drag in
// this default kickos_init_entry.

#include <kickos/sys/init.h>

extern "C" int kickos_init_entry(int argc, char** argv)
{
    // Bring the console up first. On a board with a userspace console driver this
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
