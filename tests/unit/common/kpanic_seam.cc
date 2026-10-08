// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The kernel's panic terminal for a host gate. The line goes to stderr, where a death test
// reads it (KICKOS_EXPECT_PANIC matches the prefix), and the exit is a failure outside one.

#include <stdio.h>
#include <stdlib.h>

#include <kickos/kernel.h>

namespace kickos
{
    void kpanic(char const* msg)
    {
        fprintf(stderr, "KERNEL PANIC: %s\n", msg);
        exit(1);
    }

#if KICKOS_DIAG_TERSE
    void kpanic_at(char const* file, unsigned line)
    {
        fprintf(stderr, "KERNEL PANIC: %s:%u\n", file, line);
        exit(1);
    }
#endif
}
