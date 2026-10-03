// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The init's priority witness (docs/design-m10-target.md, section 8), one core. The pulse sleeps
// and ends, and the init restarts it once; a death reaches the pulse's watchers only once the
// init has run. `high`, above the init, spins across the pulse's first end and must see no death
// arrive. `low`, below it, spins across the second and must see it arrive, the init preempting
// it. Each prints its verdict, and `low`'s exit status ends the system: 0 on the arrival.

#include <kickos/sys.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

namespace
{
    constexpr uint64_t PULSE_NS = 200000000ull;
    // Past the pulse's first end, which falls within it.
    constexpr uint64_t HIGH_SPIN_NS = 600000000ull;
    // Past the restarted pulse's end.
    constexpr uint64_t LOW_SPIN_NS = 2000000000ull;

    // The pulse's deaths as the init last wrote them, or -1 where the record cannot be read.
    int pulse_deaths(kos_self_t const* self)
    {
        struct kos_task_status st;
        if (kos_task_status(self, 0, &st) != 0)
        {
            return -1;
        }
        return st.deaths;
    }
}

extern "C" void prio_pulse_main(kos_self_t const* self)
{
    (void)self;
    kos_sleep_ns(PULSE_NS);
}

extern "C" void prio_high_main(kos_self_t const* self)
{
    int const first = pulse_deaths(self);
    int last = first;
    uint64_t const end = kos_clock_now() + HIGH_SPIN_NS;
    while (kos_clock_now() < end)
    {
        last = pulse_deaths(self);
    }
    if (first == 0 and last == 0)
    {
        printf("prio: the init stayed below a task above it\n");
        return;
    }
    printf("prio: a task above the init read %d then %d deaths\n", first, last);
}

extern "C" void prio_low_main(kos_self_t const* self)
{
    int const first = pulse_deaths(self);
    int now = first;
    uint64_t const end = kos_clock_now() + LOW_SPIN_NS;
    while (now == first and kos_clock_now() < end)
    {
        now = pulse_deaths(self);
    }
    if (first == 1 and now == 2)
    {
        printf("prio: the init preempted a task below it\n");
        return;
    }
    printf("prio: a task below the init read %d then %d deaths\n", first, now);
    exit(1);
}
