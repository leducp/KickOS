// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The priority witness's `high`, above the init: it spins across the pulse's first end and must
// see no death arrive.

#include "priority.h"

#include <stdio.h>

int main(int, char**)
{
    using namespace priority_witness;
    int const first = pulse_deaths();
    int last = first;
    uint64_t const end = kos_clock_now() + HIGH_SPIN_NS;
    while (kos_clock_now() < end)
    {
        last = pulse_deaths();
    }
    if (first == 0 and last == 0)
    {
        printf("prio: the init stayed below a task above it\n");
        return 0;
    }
    printf("prio: a task above the init read %d then %d deaths\n", first, last);
}
