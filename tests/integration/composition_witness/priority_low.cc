// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The priority witness's `low`, below the init: it spins across the pulse's second end and must
// see the death arrive, the init preempting it. Its exit status ends the system: 0 on the arrival.

#include "priority.h"

#include <stdio.h>
#include <stdlib.h>

int main(int, char**)
{
    using namespace priority_witness;
    int const first = pulse_deaths();
    int now = first;
    uint64_t const end = kos_clock_now() + LOW_SPIN_NS;
    while (now == first and kos_clock_now() < end)
    {
        now = pulse_deaths();
    }
    if (first == 1 and now == 2)
    {
        printf("prio: the init preempted a task below it\n");
        return 0;
    }
    printf("prio: a task below the init read %d then %d deaths\n", first, now);
    exit(1);
}
