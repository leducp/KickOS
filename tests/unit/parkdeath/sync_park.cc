// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The synchronisation parks: a cancel their prologue honours must leave the capability sweep
// exactly as preemptible as an ordinary exit leaves it.

#include <kickos/irqlock.h>
#include <kickos/sync.h>

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

            Semaphore g_sem;
            Mutex g_mutex;

            // The dispatch's own shape: resolve and use under one bracket, handed down.
            void wait_on_the_semaphore()
            {
                IrqLock lock;
                sem_wait(lock, &g_sem);
            }

            void lock_the_mutex()
            {
                (void)mutex_lock(&g_mutex);
            }
        }

        TEST_F(CancelledSyncPark, a_semaphore_wait_sweeps_as_an_exit_does)
        {
            Sweep const reference = reference_sweep();
            sem_init(&g_sem, 0);
            Thread* const victim = cancelled_thread();
            Sweep const parked = sweep_of(wait_on_the_semaphore);
            expect_same_sweep(reference, parked, victim);
        }

        TEST_F(CancelledSyncPark, a_mutex_lock_sweeps_as_an_exit_does)
        {
            Sweep const reference = reference_sweep();
            mutex_init(&g_mutex);
            Thread* const victim = cancelled_thread();
            Sweep const parked = sweep_of(lock_the_mutex);
            expect_same_sweep(reference, parked, victim);
        }
    }
}
