// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The kernel's walks that grow with the waiters, each timed from user space at no waiter, at the
// most main's budget seats and at half that: a wake of every waiter of one word, a dead
// holder's hand-over of every record it held, and a lost mapping's scan ending every wait
// through it.

#ifndef KICKOS_USER_APPS_COMMON_USERBENCH_WALK_H
#define KICKOS_USER_APPS_COMMON_USERBENCH_WALK_H

#include <stdint.h>

namespace bench
{
    // One row per walk, its slope against the waiters. `samples` holds 16 where the clock is fine.
    void walks(uint32_t* samples, uint32_t tare);
}

#endif
