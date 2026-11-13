// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The console restart witness's writer: LINES numbered lines on stdout, PACE apart, each one
// write, then its return ends the system.

#include <kickos/sys.h>

#include <stdint.h>
#include <stdio.h>

namespace
{
    constexpr int LINES = 30;
    constexpr int64_t PACE_MS = 20;
}

int main(int, char**)
{
    for (int k = 1; k <= LINES; k++)
    {
        printf("conwit: line %d out\n", k);
        kos_sleep_for(kos_ms(PACE_MS));
    }
}
