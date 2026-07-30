// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The default init body: narrow root's authority to what the app declared, then run
// the app's main. This TU is NOT compiled with -Dmain (it does not link kickos_core),
// so app.h's #ifdef main weak-stamp block never fires here.

#include <kickos/sys.h>           // kos_cap_narrow
#include <kickos/sys/abi.h>       // KOS_AUTH_*
#include <kickos/sys/cap_index.h> // KOS_CAP_AUTHORITY
#include <kickos/sys/errno.h>     // KOS_EBADF
#include <kickos/sys/init.h>

#include <kickos/app.h>

extern "C" int kickos_default_init_run(int argc, char** argv)
{
    // The narrow lives HERE rather than in the default kickos_init_entry so that a
    // custom provider delegating to this body cannot end up running the app with root's
    // full authority. Getting the composition wrong now costs a loud -KOS_EPERM from
    // whatever bring-up runs after this, instead of an unconfined app and no diagnostic.
    int const narrow_rc = kos_cap_narrow(KOS_CAP_AUTHORITY, kickos_app_authority());
    // -KOS_EBADF is the privileged-root answer -- kmain seats no authority cap there,
    // so the slot is empty and there is nothing to narrow. Any other refusal is a
    // kernel-side bug and aborts the app rather than running it unconfined.
    if (narrow_rc != 0 and narrow_rc != -KOS_EBADF)
    {
        return narrow_rc;
    }
    return kickos_app_main(argc, argv);
}
