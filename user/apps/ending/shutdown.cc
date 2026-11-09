// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sys.h>

extern "C"
{
    int __real_kos_shutdown(int status, enum kos_shutdown_outcome outcome);
    int __wrap_kos_shutdown(int status, enum kos_shutdown_outcome outcome);

    // A halt asks the bootloader first. Its refusal (no KOS_AUTH_BOOTLOADER, an AMP node that
    // does not own the reset) leaves the halt and its own answer as they were.
    int __wrap_kos_shutdown(int status, enum kos_shutdown_outcome outcome)
    {
        if (outcome == KOS_SHUTDOWN_HALT)
        {
            (void)__real_kos_shutdown(status, KOS_SHUTDOWN_BOOTLOADER);
        }
        return __real_kos_shutdown(status, outcome);
    }
}
