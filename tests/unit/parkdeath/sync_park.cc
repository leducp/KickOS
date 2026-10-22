// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The synchronisation parks: a cancel their prologue honours must leave the capability sweep
// exactly as preemptible as an ordinary exit leaves it.

#include <kickos/irqlock.h>
#include <kickos/sync.h>

#include <kickos/sys/abi.h>

#include "park_sweep.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            using namespace parksweep;

            class CancelledSyncPark : public KSeam
            {
            };

            uint32_t g_word = 0;

            void lock_the_mutex()
            {
                (void)mutex_lock(reinterpret_cast<uintptr_t>(&g_word), KOS_TIMEOUT_NONE);
            }

            void wait_on_the_word()
            {
                (void)word_wait(reinterpret_cast<uintptr_t>(&g_word), 0u, KOS_TIMEOUT_NONE);
            }
        }

        TEST_F(CancelledSyncPark, a_mutex_lock_sweeps_as_an_exit_does)
        {
            Sweep const reference = reference_sweep();
            g_word = 0;
            Thread* const victim = cancelled_thread();
            Sweep const parked = sweep_of(lock_the_mutex);
            expect_same_sweep(reference, parked, victim);
        }

        TEST_F(CancelledSyncPark, a_word_wait_sweeps_as_an_exit_does)
        {
            Sweep const reference = reference_sweep();
            g_word = 0;
            Thread* const victim = cancelled_thread();
            Sweep const parked = sweep_of(wait_on_the_word);
            expect_same_sweep(reference, parked, victim);
        }
    }
}
