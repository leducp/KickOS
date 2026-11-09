// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The bench's two walks under the kernel lock that grow with the waiters, each at its worst case:
// a dead holder's hand-over of every record it held, and a lost mapping's scan ending every wait
// through it.

#ifndef KICKOS_USER_APPS_COMMON_BENCH_OWNER_H
#define KICKOS_USER_APPS_COMMON_BENCH_OWNER_H

namespace bench
{
    // Prints one KOS_BENCH_OP_OWNER_PRINT report per walk and step, none where
    // KICKOS_BENCH_WALK_SLOTS is 0.
    void owner_death();
}

#endif
