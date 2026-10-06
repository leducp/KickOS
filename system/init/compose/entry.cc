// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A system target's init (docs/design-m10-target.md, section 5): root runs it after the app's
// constructors, over the table the system target emits.

#include "walk.h"

#include <kickos/arch/arch.h> // KICKOS_KERNEL_CORES
#include <kickos/sys/init.h>
#include <kickos/sys/table.h>

extern "C"
{
    int kickos_init_entry(int argc, char** argv)
    {
        (void)argc;
        (void)argv;
        kickos::init::Build build;
        build.translating = KICKOS_HAVE_ASPACE != 0;
        build.sp_masked = KICKOS_TLS != 0 and KICKOS_TLS_FROM_SP != 0;
        build.multicore = KICKOS_KERNEL_CORES > 1;
        kickos::init::Walk walk{kickos_table, build};
        walk.run();
    }
}
