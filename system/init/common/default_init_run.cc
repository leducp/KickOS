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
    // Root's authority seat is unconditional: any refusal here is a kernel-side bug, and
    // tolerating it would run the app with zero authority.
    if (narrow_rc != 0)
    {
        return narrow_rc;
    }
    int const rc = kickos_app_main(argc, argv);
    // A zero-length send on the console endpoint means FLUSH. A returning main lands in
    // kos_shutdown, which masks interrupts and halts on whatever a userspace driver's TX
    // ring still holds; console_tx_flush_sync cannot reach those bytes, they are not in
    // the kernel ring. -KOS_EBADF (nothing published) needs no handling: there is then
    // nothing to drain.
    (void)kos_send(KOS_CAP_STDOUT, "", 0);
    // TWICE, and the second one is the point. A plain send is released when the receiver
    // TAKES it, not when it finishes, so the first probe only STARTS the drain. The
    // second cannot be taken until the service thread is back in kos_recv, which it
    // reaches only after the first flush returned, so this send completing IS the drained
    // signal. There is no reply channel to use instead: kos_call is refused to a non-pool
    // caller.
    (void)kos_send(KOS_CAP_STDOUT, "", 0);
    return rc;
}
