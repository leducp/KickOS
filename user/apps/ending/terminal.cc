// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sys.h>
#include <kickos/sys/image.h>

// Root runs the init image's constructors before the init starts any task.
KOS_IMAGE_INIT_OBJECT()

namespace
{
    // Refused on an AMP node that does not own the reset, whose panic still halts.
    __attribute__((constructor)) void terminal_bootloader()
    {
        (void)kos_terminal_outcome(KOS_SHUTDOWN_BOOTLOADER);
    }
}
