// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The default system's main faulting: the task ends with KOS_EXIT_FAULT, which ends the system
// with that status.

#include <kickos/sys/trap.h>

#include <stdio.h>

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    printf("sysdefault: main faults\n");
    fflush(stdout);
    // Not a store to address 0, which no unit faults where none protects it.
    KOS_TRAP_ILLEGAL();
    return 0;
}
