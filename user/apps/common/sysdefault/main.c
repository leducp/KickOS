// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A plain C main linked against KickOS::kernel and KickOS::system_default: the default
// composition runs it as its `main` task through kickos_main, and its return ends the system
// with its status (docs/design-m10-target.md, section 8, "The default system").

#include <stdio.h>

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    printf("sysdefault: main returns 3\n");
    return 3;
}
