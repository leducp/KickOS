// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The console transport under TWO KERNEL CORES on the host: a per-thread interrupt mask, a
// real cross-core kernel lock that excludes, and a mock TX edge that records which core
// pushed each byte and who held the lock when it did.
//
// The mask is per THREAD, so an arm speaks as whichever core it names, and a producer's mask
// therefore reaches only its own core. That is the posture the drain's exclusion has to hold
// in, and the one tests/unit/consoletx cannot express: its mock runs the drain in the
// producer's own mask gap, which is the single-core machine.

#ifndef KICKOS_TESTS_UNIT_CONSOLETXSMP_TX_SMP_SEAM_H
#define KICKOS_TESTS_UNIT_CONSOLETXSMP_TX_SMP_SEAM_H

#include <stdint.h>

#include <string>

namespace consoletxsmp
{
    // Which core arch_cpu_id answers, per thread. klock keys its per-core row off this.
    extern thread_local uint32_t g_core;

    // Attach the mock to a ring of `ring_size` bytes, armed at IRQ line 3, and clear every
    // counter.
    void reset(uint32_t ring_size);

    // Bytes the mock TX edge accepted, in wire order.
    std::string wire();

    // Of those, how many that core pushed.
    uint32_t pushes_by(uint32_t core);

    // Whether any push was made by a core other than the one holding the kernel lock. That
    // is two writers of the ring's indices overlapping, read at the device.
    bool push_outside_the_lock();

    // Whether a claim on the kernel lock ever failed since reset, so a core genuinely waited
    // for another. An arm whose peer never contended proves nothing about exclusion.
    bool lock_contended();

    // Run `fn` once, inside that core's NEXT push to the TX edge, after the byte is taken.
    void hold_at_push(uint32_t core, void (*fn)(void));
    bool hold_fired();

    // Spin until `pred` answers true, up to the hand-off budget. False means the budget blew,
    // which an arm must report as a verdict rather than hang on.
    bool wait_for(bool (*pred)(void));
}

#endif
