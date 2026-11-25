// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A task that holds a crossing's port and never receives on it.

#include <iso646.h> // and / or / not are macros in C, not keywords
#include <stdbool.h>

#include <kickos/sys.h>

int main(void)
{
    while (true)
    {
        kos_sleep_for(kos_s(1));
    }
}
