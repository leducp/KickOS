// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A death honoured at a park prologue, measured against an ordinary exit of an identical
// thread. The seam traces a gap only where its bracket count returns to zero, so the gap count
// of a sweep is the number of points at which it really was preemptible.

#ifndef KICKOS_TESTS_UNIT_PARKDEATH_PARK_SWEEP_H
#define KICKOS_TESTS_UNIT_PARKDEATH_PARK_SWEEP_H

#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/thread.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace parksweep
        {
            constexpr int SLOT_BYSTANDER = 0;
            constexpr int SLOT_REFERENCE = 1;
            constexpr int SLOT_CANCELLED = 2;
            constexpr uint8_t PRIO_BYSTANDER = 6;
            constexpr uint8_t PRIO_DYING = 5;
            // A chunk gap, past the one exit_current opens ahead of the sweep.
            constexpr uint32_t GAP_PROBED = 2;
            constexpr int LOCK_UNREAD = -1;

            struct Sweep
            {
                uint32_t gaps;
                uint32_t depth_after;
                int lock_in_gap;
            };

            inline int g_lock_in_gap = LOCK_UNREAD;

            inline void probe_gap()
            {
#if KICKOS_KERNEL_CORES > 1
                g_lock_in_gap = 0;
                if (klock_held())
                {
                    g_lock_in_gap = 1;
                }
#else
                g_lock_in_gap = 0;
#endif
            }

            inline void seat_running(Thread* t)
            {
#if KICKOS_KERNEL_CORES > 1
                g_core = 0;
                seat_running_on(t, 0);
#else
                kernel().current[0] = t;
                t->state = ThreadState::RUNNING;
#endif
            }

            inline Thread* dying_thread(int slot)
            {
                Thread* const t = seat_pool(slot, PRIO_DYING);
                attach_caps(t, KICKOS_CAP_CHILD_WIDTH);
                seat_running(t);
                return t;
            }

            inline Sweep sweep_of(void (*death)())
            {
                g_lock_in_gap = LOCK_UNREAD;
                trace_reset();
                run_in_chunk_gap(probe_gap, GAP_PROBED);
                run_noreturn(death);
                return Sweep{gaps_seen(), irq_depth(), g_lock_in_gap};
            }

            inline void exit_plainly()
            {
                sched::exit_current(0, sched::EXIT_RETURN);
            }

            // The reference is the path that holds nothing on entry.
            inline Sweep reference_sweep()
            {
                (void)seat_pool(SLOT_BYSTANDER, PRIO_BYSTANDER);
                Thread* const t = dying_thread(SLOT_REFERENCE);
                Sweep const s = sweep_of(exit_plainly);
                EXPECT_EQ(t->state, ThreadState::EXITED);
                return s;
            }

            inline Thread* cancelled_thread()
            {
                Thread* const t = dying_thread(SLOT_CANCELLED);
                IrqLock lock;
                thread_cancel(t);
                return t;
            }

            inline void expect_same_sweep(Sweep const& reference, Sweep const& parked,
                                          Thread const* victim)
            {
                ASSERT_GT(reference.gaps, GAP_PROBED)
                    << "the reference sweep opened too few gaps for the arm to compare";
                ASSERT_NE(reference.lock_in_gap, LOCK_UNREAD);
                EXPECT_EQ(victim->state, ThreadState::EXITED);
                EXPECT_EQ(parked.gaps, reference.gaps)
                    << "a cancel honoured at a park prologue swept with fewer preemption "
                       "points than an ordinary exit";
                EXPECT_EQ(parked.depth_after, 0u)
                    << "the park's own bracket outlived the death";
                EXPECT_EQ(parked.lock_in_gap, 0)
                    << "the kernel lock was held across a chunk gap";
            }
        }
    }
}

#endif
