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
    constexpr uint64_t PACE_NS = 20000000ull; // 20 ms
}

int main(int, char**)
{
    for (int k = 1; k <= LINES; k++)
    {
        printf("conwit: line %d out\n", k);
        kos_sleep_ns(PACE_NS);
    }
}
