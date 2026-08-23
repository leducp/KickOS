// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The fault-attribution band. kickos_fault_below_stack decides whether a denied access is
// "this thread ran off the bottom of its own stack" (escalate to the panic dump) or "this
// thread touched someone else's memory" (kill the thread alone), and kickos_fault_stack_top
// is where a backend puts the stub so it never runs on the exhausted stack.
//
// Two rx72m silicon captures pin the CONTRACT without pinning the WIDTH:
// faultsurvive_ovf's MPDEA=0x121fc is its faulter's stack_base - 4 and must escalate, and
// mpu_fault's 0x13200 is a cross-domain write far below domainA's stack base and must die
// alone. Every arm therefore MEASURES the band off the predicate instead of naming it, so
// what the arms pin is the rule and not the bench's tuning.

#include <kickos/arch/arch.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class FaultBand : public KSeam
            {
            };

            constexpr uint8_t PRIO = 5;
            // Far from 0 so the underflow arm below is the only one that can reach a wrap.
            constexpr uintptr_t STACK_BASE = 0x20010000u;
            constexpr size_t STACK_SIZE = 0x800u;

            Thread* seat_current_with_stack(uintptr_t base, size_t size)
            {
                Thread* const t = spawn(0, PRIO);
                t->stack_base = reinterpret_cast<void*>(base);
                t->stack_size = size;
                kernel().current = t;
                return t;
            }

            // fault.cc keeps the width file-local, so the fixture derives it. The ceiling
            // refuses a predicate that answers true forever.
            constexpr uintptr_t BAND_PROBE_CEILING = 0x10000u;

            // The largest d for which base - d still reads as an overflow.
            uintptr_t measured_band(uintptr_t base)
            {
                uintptr_t last = 0;
                for (uintptr_t d = 1; d <= BAND_PROBE_CEILING; d++)
                {
                    if (kickos_fault_below_stack(base - d))
                    {
                        last = d;
                    }
                    else
                    {
                        break;
                    }
                }
                return last;
            }

            TEST_F(FaultBand, an_address_inside_the_stack_is_not_below_it)
            {
                seat_current_with_stack(STACK_BASE, STACK_SIZE);
                EXPECT_FALSE(kickos_fault_below_stack(STACK_BASE));
                EXPECT_FALSE(kickos_fault_below_stack(STACK_BASE + 4));
                EXPECT_FALSE(kickos_fault_below_stack(STACK_BASE + STACK_SIZE - 4));
            }

            // The faultsurvive_ovf shape: RXv3 cancels the faulting instruction and restores
            // SP, so the denied PUSH is the only evidence an overflow happened at all.
            TEST_F(FaultBand, the_denied_push_just_under_the_base_is_an_overflow)
            {
                seat_current_with_stack(STACK_BASE, STACK_SIZE);
                // 4 is the measured shape (faultsurvive_ovf's MPDEA is its faulter's
                // stack_base - 4), and a band narrower than that stops attributing the one
                // fault RXv3 gives no other evidence of.
                EXPECT_TRUE(kickos_fault_below_stack(STACK_BASE - 4));
                uintptr_t const w = measured_band(STACK_BASE);
                EXPECT_GE(w, 4u);
                EXPECT_LT(w, BAND_PROBE_CEILING) << "the band is unbounded: every address below "
                                                    "the stack reads as an overflow, which is "
                                                    "the pre-narrowing behaviour";
                EXPECT_TRUE(kickos_fault_below_stack(STACK_BASE - w));
            }

            // The mpu_fault shape, and the arm the NARROWING is for: a cross-domain write
            // below the band kills its thread instead of escalating.
            TEST_F(FaultBand, a_cross_domain_write_below_the_band_is_not_an_overflow)
            {
                seat_current_with_stack(STACK_BASE, STACK_SIZE);
                uintptr_t const w = measured_band(STACK_BASE);
                EXPECT_FALSE(kickos_fault_below_stack(STACK_BASE - w - 1));
                EXPECT_FALSE(kickos_fault_below_stack(STACK_BASE - 0x1000u));
            }

            // An address at or above the base is never an overflow, whatever the base is. This
            // early return is what makes the two wrap arms below the ONLY paths into the band
            // arithmetic.
            TEST_F(FaultBand, a_high_address_is_not_an_overflow_of_a_low_stack)
            {
                seat_current_with_stack(4u, STACK_SIZE);
                EXPECT_FALSE(kickos_fault_below_stack(UINTPTR_MAX));
                EXPECT_FALSE(kickos_fault_below_stack(UINTPTR_MAX - 8u));
            }

            // THE SPELLING ARM. `addr >= base - BAND` and `addr + BAND >= base` agree
            // everywhere except where the arithmetic wraps, so only a stack based within BAND
            // of the top of the address space tells them apart, and the second spelling then
            // reports a genuine overflow as a cross-domain write, killing a thread whose real
            // problem is that the image under-provisioned its stack.
            //
            // Reaching it needs `addr < base` AND `addr + BAND` to wrap, so both have to sit in
            // the top BAND bytes. That is why the low-base arm above cannot catch this: its
            // early return fires first.
            TEST_F(FaultBand, the_band_is_subtracted_from_the_base_not_added_to_the_address)
            {
                constexpr uintptr_t HIGH_BASE = UINTPTR_MAX - 2u;
                seat_current_with_stack(HIGH_BASE, STACK_SIZE);
                EXPECT_TRUE(kickos_fault_below_stack(HIGH_BASE - 1u));
                EXPECT_TRUE(kickos_fault_below_stack(UINTPTR_MAX - 8u));
            }

            // The other end of the same arithmetic: `base - BAND` underflows when the stack
            // sits inside the first band, and an unguarded subtraction then compares against a
            // near-UINTPTR_MAX bound that nothing is above, so NO overflow would ever be
            // attributed on such a thread. Fails closed instead.
            TEST_F(FaultBand, a_stack_inside_the_first_band_still_attributes_its_overflow)
            {
                seat_current_with_stack(4u, STACK_SIZE);
                EXPECT_TRUE(kickos_fault_below_stack(0u));
                EXPECT_TRUE(kickos_fault_below_stack(3u));
            }

            TEST_F(FaultBand, the_stub_lands_at_the_top_of_the_dying_threads_own_stack)
            {
                seat_current_with_stack(STACK_BASE, STACK_SIZE);
                EXPECT_EQ(kickos_fault_stack_top(), STACK_BASE + STACK_SIZE);
            }

            // 0 is the backend's "do not relocate" answer, and it has to be reachable: a
            // thread with no stack of its own is exactly the case where a reset would send the
            // stub to an address the thread does not own.
            TEST_F(FaultBand, a_thread_with_no_stack_declines_the_reset)
            {
                Thread* const t = spawn(0, PRIO);
                t->stack_base = nullptr;
                t->stack_size = 0;
                kernel().current = t;
                EXPECT_EQ(kickos_fault_stack_top(), 0u);

                t->stack_base = reinterpret_cast<void*>(STACK_BASE);
                t->stack_size = 0;
                EXPECT_EQ(kickos_fault_stack_top(), 0u);
            }

            // Both predicates read sched::current(), so both owe an answer when there is none
            // (a fault before the first thread runs is a real boot case) and they answer
            // in OPPOSITE directions because they are asked different questions. Each answers
            // the SAFE way for its own question, and this arm is what stops either from being
            // "simplified" into the other's default.
            //
            //   below_stack -> true:  ESCALATE. An unattributable fault must reach the panic
            //                         dump, never be charged to a thread that may not own it.
            //   stack_top   -> 0:     DO NOT RELOCATE. There is no stack known to be the
            //                         thread's own, so a reset would aim the stub at memory
            //                         nobody established it may use.
            TEST_F(FaultBand, an_unattributable_fault_escalates_and_relocates_nothing)
            {
                kernel().current = nullptr;
                EXPECT_TRUE(kickos_fault_below_stack(STACK_BASE - 4));
                EXPECT_EQ(kickos_fault_stack_top(), 0u);
            }

            // Same rule, reached the other way: a thread whose stack is not known escalates
            // too.
            TEST_F(FaultBand, a_thread_with_no_stack_escalates_rather_than_dying_alone)
            {
                Thread* const t = spawn(0, PRIO);
                t->stack_base = nullptr;
                t->stack_size = 0;
                kernel().current = t;
                EXPECT_TRUE(kickos_fault_below_stack(STACK_BASE - 4));
            }
        }
    }
}
