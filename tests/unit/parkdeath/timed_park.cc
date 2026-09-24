// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The timed park, over the real kernel/time/time.cc: a cancel its prologue honours must leave
// the capability sweep exactly as preemptible as an ordinary exit leaves it.

#include <kickos/time.h>

#include "park_sweep.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            using namespace parksweep;

            class CancelledTimedPark : public KSeam
            {
            };

            constexpr uint64_t SLEEP_NS = 1000000u;

            void sleep_a_while()
            {
                ktime_sleep_until(g_now_ns + SLEEP_NS);
            }
        }

        TEST_F(CancelledTimedPark, a_sleep_sweeps_as_an_exit_does)
        {
            Sweep const reference = reference_sweep();
            Thread* const victim = cancelled_thread();
            Sweep const parked = sweep_of(sleep_a_while);
            expect_same_sweep(reference, parked, victim);
            EXPECT_EQ(kernel().sleepq, nullptr) << "the cancelled sleeper was queued";
        }
    }
}
