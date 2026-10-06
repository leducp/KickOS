// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The default system's main faulting: the task ends with KOS_EXIT_FAULT, which ends the system
// with that status.

#include <stdio.h>

int main(int argc, char** argv)
{
    // volatile: an object the compiler must not elide, or the write through it would fold away.
    volatile int* volatile nowhere = 0;

    (void)argc;
    (void)argv;
    printf("sysdefault: main faults\n");
    fflush(stdout);
    *nowhere = 1;
    return 0;
}
