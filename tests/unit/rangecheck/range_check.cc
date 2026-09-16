// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The combined read-AND-write range check against the two separate ones it replaces. The
// merged walk is an isolation decision, so what is gated here is EQUIVALENCE and not
// plausibility: every arm names the reference verdict
// `user_readable_ok(p, r) and user_writable_ok(p, w)` and compares against it.
//
// Two mutations leave the kernel booting and are what the named arms are shaped around: an
// OR instead of an AND, which hands a caller a reply destination it may only read, and one
// length standing in for the other, which admits a write past the readable extent.
//
// BUILT TWICE, ONCE PER POSTURE THE CHECKS SHIP AT. Where the backend translates, the walk
// consults the domain's granted-range list as well as the region set, and that half is
// `#if KICKOS_HAVE_ASPACE`: read at the preset's own posture alone, the equivalence claim
// would stand over source text the compiler never saw. The arms and the sweep below are
// therefore written so the list is one more answerer, and its half compiles away with it.
//
// The arch hooks are answered from this TU so an arm can move the granted set and the
// static-extent fallback independently.

#include <stddef.h>
#include <stdint.h>

#include <kickos/arch/arch.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#if KICKOS_HAVE_ASPACE
#include <kickos/domain.h>
#include <kickos/task.h>
#include <kickos/vrange.h>

#include "aspace_seam.h"
#endif

#include "kseam_test.h"
#include "syscall_internal.h"

namespace
{
    bool g_text_readable = false;
    bool g_data_writable = false;
}

extern "C"
{
    bool arch_user_text_readable(uintptr_t, size_t)
    {
        return g_text_readable;
    }

    bool arch_user_data_writable(uintptr_t, size_t)
    {
        return g_data_writable;
    }
}

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            constexpr int SLOT_CALLER = 0;
            constexpr uint8_t PRIO = 4;

            // A window of addresses, named and never dereferenced.
            constexpr uintptr_t BASE = 0x20000000u;
            constexpr size_t SPAN = 0x1000u;
            // The range list names the same window the two regions below cover, one granule
            // each.
            constexpr size_t VPAGES = 2u;
            // The size of a descriptor seated AT BASE whose end wraps, landing a few bytes
            // above zero. Seated at that base, no pointer this gate asks about is below it,
            // so the walk refuses it on the wrapped end alone: a base above BASE, or a size
            // that stops short of the wrap, makes every arm that seats it vacuous.
            constexpr size_t WRAP_SPAN = static_cast<size_t>(0) - BASE + 8u;

            class RangeCheck : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    g_text_readable = false;
                    g_data_writable = false;
                    caller_ = seat_pool(SLOT_CALLER, PRIO);
                    kernel().current[kickos_kernel_core()] = caller_;
#if KICKOS_HAVE_ASPACE
                    ASSERT_EQ(arch_aspace_granule(), SPAN)
                        << "one region of this gate is one granule, which is what makes a "
                           "two-page range the window the two regions cover";
                    Task* const tk = task(0);
                    join_task(caller_, tk);
                    domain_ = task_domain(tk);
                    ranges_ = seat_space(domain_);
                    ASSERT_NE(ranges_, nullptr)
                        << "without a space the domain holds no list and the translating half "
                           "of the walk is never entered";
#endif
                }

                void grant(uintptr_t base, size_t size, uint32_t attr)
                {
                    ASSERT_TRUE(caller_->mpu.add(base, size, attr))
                        << "the set must hold every region an arm seats";
                }

                // The list the translating half consults, re-seated from scratch: one
                // reservation over the window the regions cover, granted with `rights`. 0
                // leaves it RESERVED, which admits nothing.
                void seat_list(uint32_t rights)
                {
#if KICKOS_HAVE_ASPACE
                    ASSERT_TRUE(ranges_->init(arch_aspace_granule()));
                    ASSERT_TRUE(ranges_->reserve(BASE, VPAGES));
                    if (rights != 0)
                    {
                        ASSERT_TRUE(ranges_->grant(BASE, VPAGES, rights,
                                                   static_cast<uint8_t>(ARCH_MAP_NORMAL)));
                    }
#else
                    (void)rights;
#endif
                }

                // What the pair of separate checks answers. Every expectation below is
                // against this and never against a hand-written truth value.
                static bool separately(uintptr_t p, size_t r, size_t w)
                {
                    return user_readable_ok(p, r) and user_writable_ok(p, w);
                }

                static void expect_agrees(uintptr_t p, size_t r, size_t w)
                {
                    EXPECT_EQ(user_readable_and_writable_ok(p, r, w), separately(p, r, w))
                        << "combined and separate disagree at base " << p << " read " << r
                        << " write " << w;
                }

                Thread* caller_ = nullptr;
#if KICKOS_HAVE_ASPACE
                Domain* domain_ = nullptr;
                VirtualRanges* ranges_ = nullptr;
#endif
            };

            TEST_F(RangeCheck, one_rw_region_admits_both_halves)
            {
                grant(BASE, SPAN, ARCH_MPU_R | ARCH_MPU_W);
                EXPECT_TRUE(user_readable_and_writable_ok(BASE, 64, 64));
                expect_agrees(BASE, 64, 64);
            }

            // THE OR MUTATION. A readable-only buffer passes the read half, and a merged
            // check that answered "readable or writable" would let the kernel write it.
            TEST_F(RangeCheck, a_read_only_region_refuses_the_pair)
            {
                grant(BASE, SPAN, ARCH_MPU_R);
                EXPECT_TRUE(user_readable_ok(BASE, 64)) << "the read half is what an OR takes";
                EXPECT_FALSE(user_readable_and_writable_ok(BASE, 64, 64));
                expect_agrees(BASE, 64, 64);
            }

            TEST_F(RangeCheck, a_write_only_region_refuses_the_pair)
            {
                grant(BASE, SPAN, ARCH_MPU_W);
                EXPECT_FALSE(user_readable_and_writable_ok(BASE, 64, 64));
                expect_agrees(BASE, 64, 64);
            }

            // Two regions, each carrying one half. The separate checks accept, so the merged
            // walk must too: a merge that demanded ONE region carrying both bits would refuse
            // a range the caller can genuinely read and write.
            TEST_F(RangeCheck, the_two_halves_may_come_from_different_regions)
            {
                grant(BASE, SPAN, ARCH_MPU_R);
                grant(BASE, SPAN, ARCH_MPU_W);
                EXPECT_TRUE(separately(BASE, 64, 64));
                EXPECT_TRUE(user_readable_and_writable_ok(BASE, 64, 64));
            }

            // THE ONE-LENGTH MUTATION. The write half is longer than the region, and only a
            // check that measures each half on its own length refuses it.
            TEST_F(RangeCheck, each_half_is_measured_on_its_own_length)
            {
                grant(BASE, SPAN, ARCH_MPU_R | ARCH_MPU_W);
                EXPECT_TRUE(user_readable_and_writable_ok(BASE, SPAN, SPAN));
                EXPECT_FALSE(user_readable_and_writable_ok(BASE, SPAN, SPAN + 1));
                EXPECT_FALSE(user_readable_and_writable_ok(BASE, SPAN + 1, SPAN));
                expect_agrees(BASE, SPAN, SPAN + 1);
                expect_agrees(BASE, SPAN + 1, SPAN);
            }

            TEST_F(RangeCheck, a_zero_length_half_asks_the_set_nothing)
            {
                grant(BASE, SPAN, ARCH_MPU_R);
                EXPECT_TRUE(user_readable_and_writable_ok(BASE, 64, 0));
                expect_agrees(BASE, 64, 0);
                expect_agrees(BASE, 0, 64);
                expect_agrees(BASE, 0, 0);
            }

            TEST_F(RangeCheck, an_extent_that_wraps_is_refused)
            {
                grant(BASE, SPAN, ARCH_MPU_R | ARCH_MPU_W);
                size_t const huge = static_cast<size_t>(0) - BASE;
                EXPECT_FALSE(user_readable_and_writable_ok(BASE, huge, 64));
                EXPECT_FALSE(user_readable_and_writable_ok(BASE, 64, huge));
                expect_agrees(BASE, huge, 64);
                expect_agrees(BASE, 64, huge);
            }

            // A DESCRIPTOR WHOSE END WRAPS. Asserted absolutely and not only against the
            // reference: the two walks spell the same guard, so a clamp written into both
            // would read this region as reaching the top of memory and still agree.
            TEST_F(RangeCheck, a_region_whose_end_wraps_covers_nothing)
            {
                grant(BASE, WRAP_SPAN, ARCH_MPU_R | ARCH_MPU_W);
                EXPECT_FALSE(user_readable_ok(BASE, 64));
                EXPECT_FALSE(user_writable_ok(BASE, 64));
                EXPECT_FALSE(user_readable_and_writable_ok(BASE, 64, 64));
                expect_agrees(BASE, 64, 64);
            }

            TEST_F(RangeCheck, a_privileged_caller_passes_without_a_region)
            {
                caller_->privileged = true;
                EXPECT_TRUE(user_readable_and_writable_ok(BASE, 64, 64));
                expect_agrees(BASE, 64, 64);
            }

            // NO CURRENT THREAD AT ALL, which every arm above seats past. The merged walk has
            // no set and no privilege posture to read, so both halves fall through to their
            // own arch hook; a walk that admitted a caller it cannot name would answer true
            // where the separate pair answers false.
            TEST_F(RangeCheck, no_current_thread_falls_through_to_both_hooks)
            {
                grant(BASE, SPAN, ARCH_MPU_R | ARCH_MPU_W);
                seat_list(ARCH_MAP_R | ARCH_MAP_W);
                kernel().current[kickos_kernel_core()] = nullptr;
                for (int hooks = 0; hooks < 4; hooks++)
                {
                    g_text_readable = ((hooks & 1) != 0);
                    g_data_writable = ((hooks & 2) != 0);
                    EXPECT_EQ(user_readable_and_writable_ok(BASE, 64, 64),
                              g_text_readable and g_data_writable);
                    expect_agrees(BASE, 64, 64);
                    expect_agrees(BASE, 0, 64);
                    expect_agrees(BASE, 64, 0);
                }
            }

            // Where the set describes nothing the backend's static extent is the whole
            // answer, and the two hooks are separate answers: a readable text extent must not
            // carry the write half.
            TEST_F(RangeCheck, the_static_extent_answers_each_half_on_its_own)
            {
                g_text_readable = true;
                g_data_writable = false;
                EXPECT_FALSE(user_readable_and_writable_ok(BASE, 64, 64));
                expect_agrees(BASE, 64, 64);
                g_data_writable = true;
                EXPECT_TRUE(user_readable_and_writable_ok(BASE, 64, 64));
                expect_agrees(BASE, 64, 64);
            }

            // One half from the granted set and the other from the static extent.
            TEST_F(RangeCheck, the_two_halves_may_come_from_different_answerers)
            {
                grant(BASE, SPAN, ARCH_MPU_R);
                g_text_readable = false;
                g_data_writable = true;
                EXPECT_TRUE(separately(BASE, 64, 64));
                EXPECT_TRUE(user_readable_and_writable_ok(BASE, 64, 64));
            }

#if KICKOS_HAVE_ASPACE
            // THE TRANSLATING HALF, which is the whole answerer where the region set holds
            // nothing: the list's rights are asked one half at a time, so a range carrying
            // one of them must not admit the other. A swap of the two here is what a board
            // selftest cannot see, the call being accepted either way.
            TEST_F(RangeCheck, a_read_only_range_refuses_the_pair)
            {
                seat_list(ARCH_MAP_R);
                EXPECT_TRUE(user_readable_ok(BASE, 64)) << "the read half is what a swap takes";
                EXPECT_FALSE(user_readable_and_writable_ok(BASE, 64, 64));
                expect_agrees(BASE, 64, 64);
            }

            TEST_F(RangeCheck, a_write_only_range_refuses_the_pair)
            {
                seat_list(ARCH_MAP_W);
                EXPECT_FALSE(user_readable_ok(BASE, 64));
                EXPECT_TRUE(user_writable_ok(BASE, 64));
                EXPECT_FALSE(user_readable_and_writable_ok(BASE, 64, 64));
                expect_agrees(BASE, 64, 64);
            }

            // One half from the region set and the other from the list.
            TEST_F(RangeCheck, the_two_halves_may_come_from_the_set_and_the_list)
            {
                grant(BASE, SPAN, ARCH_MPU_R);
                seat_list(ARCH_MAP_W);
                EXPECT_TRUE(separately(BASE, 64, 64));
                EXPECT_TRUE(user_readable_and_writable_ok(BASE, 64, 64));
            }

            // A domain holding no space holds no list, which is the arm the `ranges != null`
            // guard exists for.
            TEST_F(RangeCheck, a_domain_holding_no_space_is_asked_nothing)
            {
                seat_list(ARCH_MAP_R | ARCH_MAP_W);
                EXPECT_TRUE(user_readable_and_writable_ok(BASE, 64, 64));
                drop_space(domain_);
                EXPECT_FALSE(user_readable_and_writable_ok(BASE, 64, 64));
                expect_agrees(BASE, 64, 64);
            }
#endif

            // The sweep the named arms above are legible samples of: every combination of a
            // caller posture, a region set, the list's rights, a pair of lengths, an offset
            // and both hook answers, compared against the reference verdict.
            TEST_F(RangeCheck, the_combined_verdict_matches_the_separate_ones_everywhere)
            {
                struct Posture
                {
                    bool privileged;
                    bool wrapping;
                };
                // The named arms above seat nothing beside the posture they name: the
                // privileged one no region at all, the wrapping one no second region, no
                // list and no hook answer. Both are dimensions here so that the walk meets
                // them with every other answerer in play.
                static constexpr Posture POSTURES[] = {
                    {false, false}, {false, true}, {true, false}, {true, true}};
                static constexpr uint32_t ATTRS[] = {ARCH_MPU_R, ARCH_MPU_W,
                                                     ARCH_MPU_R | ARCH_MPU_W,
                                                     ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_DEV};
                static constexpr size_t LENS[] = {0, 1, 64, SPAN - 1, SPAN, SPAN + 1, SPAN * 4};
                static constexpr uintptr_t OFFSETS[] = {0, 8, SPAN - 8, SPAN};
                // 0 reserves the window and grants nothing, so a reservation admitting a
                // pointer is caught here too. Compiles to the single no-op case where the
                // backend translates nothing.
#if KICKOS_HAVE_ASPACE
                static constexpr uint32_t VRIGHTS[] = {0, ARCH_MAP_R, ARCH_MAP_W,
                                                       ARCH_MAP_R | ARCH_MAP_W};
#else
                static constexpr uint32_t VRIGHTS[] = {0};
#endif

                for (Posture const& posture : POSTURES)
                {
                    caller_->privileged = posture.privileged;
                    for (uint32_t vrights : VRIGHTS)
                    {
                        for (uint32_t first : ATTRS)
                        {
                            for (uint32_t second : ATTRS)
                            {
                                for (int hooks = 0; hooks < 4; hooks++)
                                {
                                    g_text_readable = ((hooks & 1) != 0);
                                    g_data_writable = ((hooks & 2) != 0);
                                    caller_->mpu.clear();
                                    if (posture.wrapping)
                                    {
                                        grant(BASE, WRAP_SPAN, ARCH_MPU_R | ARCH_MPU_W);
                                    }
                                    grant(BASE, SPAN, first);
                                    grant(BASE + SPAN, SPAN, second);
                                    seat_list(vrights);
                                    for (uintptr_t off : OFFSETS)
                                    {
                                        for (size_t r : LENS)
                                        {
                                            for (size_t w : LENS)
                                            {
                                                expect_agrees(BASE + off, r, w);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
