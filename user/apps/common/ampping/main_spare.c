// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A task that never receives: it holds a crossing's port open for amp_far_reply_empty.

#include <iso646.h> // and / or / not are macros in C, not keywords
#include <stdbool.h>

#include <kickos/sys.h>

int main(void)
{
    while (true)
    {
        kos_sleep_ns(1000000000ull);
    }
}
