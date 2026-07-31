// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The default init body: narrow root's authority to what the app declared, then run
// the app's main. This TU is NOT compiled with -Dmain (it does not link kickos_core),
// so app.h's #ifdef main build-stamp block never fires here.

#include <kickos/sys.h>           // kos_cap_narrow
#include <kickos/sys/abi.h>       // KOS_AUTH_*
#include <kickos/sys/cap_index.h> // KOS_CAP_AUTHORITY
#include <kickos/sys/init.h>

#include <kickos/app.h>

extern "C" int kickos_default_init_run(int argc, char** argv)
{
    // In the run body, not the entry, so a custom provider delegating here cannot run the
    // app with root's full authority.
    int const narrow_rc = kos_cap_narrow(KOS_CAP_AUTHORITY, kickos_app_authority());
    // Root's authority seat is unconditional, so ANY refusal here is a kernel-side bug.
    // Tolerating one would let the app run with zero authority and die later at an
    // arbitrary gate instead of at the narrow.
    if (narrow_rc != 0)
    {
        return narrow_rc;
    }
    return kickos_app_main(argc, argv);
}
