// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KickOS::main, the entry a composition names as `entry: kickos_main` to run a plain app's main
// as its task (docs/design-m10-target.md, sections 1.5 and 5).

#include <kickos/app.h> // kickos_app_main, which the build renames main to
#include <kickos/sys/init.h>

#include <stdlib.h>

extern "C" void kickos_main(struct kos_table_task const* self)
{
    (void)self;
    exit(kickos_app_main(kickos_init_args.argc, kickos_init_args.argv));
}
