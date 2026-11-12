// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A call whose out word cannot be written, over the REAL system-call, capability, notification
// and interrupt layers at the translating posture: the page holding the out word is refused
// between the call's check and its delivery, the call answers -KOS_EFAULT, and it leaves what
// it would have answered as it was before it. A mint leaves the caller's table and the object's
// pool, a create leaves no thread or task running, and a wait leaves its bits pending.

#include "../common/seam_pools.h"

#include <stdint.h>

#include <kickos/notify.h>

#include "sys_fixture.h"
#include "syscall_internal.h"

extern "C"
{
    alignas(4096) unsigned char g_sys_image[SYS_IMAGE_BYTES];
}

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            constexpr int CLAIM_LINE = 3;

            enum class Mint
            {
                NOTIFY,
                NOTIFY_BADGE,
                PROGRAM,
                IRQ,
                SPACE
            };

            class CapDeliver : public SysFixture, public ::testing::WithParamInterface<Mint>
            {
              protected:
                void SetUp() override
                {
                    SysFixture::SetUp();
                    cap_seat_authority(main_, AUTH_MEMORY | AUTH_TASKS | AUTH_IRQ);
                    uint32_t src = KCAP_INVALID;
                    ASSERT_EQ(mint(Mint::NOTIFY, &src), 0);
                    source_ = src;
                }

                // The out word sits on a page of its own, apart from the program name the
                // find reads.
                uintptr_t out() const
                {
                    return uva(EDIT_OUT);
                }

                int32_t mint(Mint m, uint32_t* cap)
                {
                    *host<uint32_t>(out()) = KCAP_INVALID;
                    uint64_t rc = 0;
                    switch (m)
                    {
                        case Mint::NOTIFY:
                        {
                            rc = call_as(main_, KOS_SYS_NOTIFY_CREATE, out());
                            break;
                        }
                        case Mint::NOTIFY_BADGE:
                        {
                            rc = call_as(main_, KOS_SYS_NOTIFY_BADGE, source_, 1u, out());
                            break;
                        }
                        case Mint::PROGRAM:
                        {
                            rc = call_as(main_, KOS_SYS_PROGRAM_FIND, uva(MAIN_OUT, PROGRAM_AT),
                                         out());
                            break;
                        }
                        case Mint::IRQ:
                        {
                            rc = call_as(main_, KOS_SYS_IRQ_CLAIM, CLAIM_LINE, 0u, out());
                            break;
                        }
                        case Mint::SPACE:
                        {
                            rc = call_as(main_, KOS_SYS_ASPACE_SELF, out());
                            break;
                        }
                    }
                    *cap = *host<uint32_t>(out());
                    return err(rc);
                }

                uint32_t usage(BudgetKind kind)
                {
                    IrqLock lock;
                    return task_usage(task_, kind);
                }

                uint32_t source_ = KCAP_INVALID;
            };

            TEST_P(CapDeliver, an_undeliverable_capability_leaves_nothing_behind)
            {
                Mint const m = GetParam();
                uint32_t cap = KCAP_INVALID;
                // A first mint closed again: an object a kind keeps per holder (the line) is
                // free to take, and its slot is the table's next.
                ASSERT_EQ(mint(m, &cap), 0);
                ASSERT_EQ(close(main_, cap), 0);
                uint32_t last = KCAP_INVALID;
                uint32_t filler = KCAP_INVALID;
                while (mint(Mint::SPACE, &filler) == 0)
                {
                    last = filler;
                }
                ASSERT_EQ(close(main_, last), 0) << "fixture: the table holds no free slot";
                uint32_t const notifies = usage(BudgetKind::NOTIFICATIONS);
                uint32_t const bindings = usage(BudgetKind::IRQ_BINDINGS);

                refuse_acquire(out(), 1);
                EXPECT_EQ(mint(m, &cap), -KOS_EFAULT);
                EXPECT_EQ(cap, KCAP_INVALID);
                EXPECT_EQ(usage(BudgetKind::NOTIFICATIONS), notifies)
                    << "the undelivered notification stayed charged";
                EXPECT_EQ(usage(BudgetKind::IRQ_BINDINGS), bindings)
                    << "the undelivered claim kept its binding";

                EXPECT_EQ(mint(m, &cap), 0) << "the undelivered capability kept its slot or its object";
                EXPECT_NE(cap, KCAP_INVALID);
            }

            INSTANTIATE_TEST_SUITE_P(EachMint, CapDeliver,
                                     ::testing::Values(Mint::NOTIFY, Mint::NOTIFY_BADGE,
                                                       Mint::PROGRAM, Mint::IRQ, Mint::SPACE));

            constexpr uint32_t BIT = 1u << 3;

            Notification* g_raised = nullptr;

            // The fixture poisons the result the park seated, which a raise leaves as it is.
            void raise_bit(Thread* parked)
            {
                parked->wait_result = 0;
                (void)notify_raise(g_raised, BIT);
            }

            class OutDeliver : public SysFixture
            {
              protected:
                void SetUp() override
                {
                    SysFixture::SetUp();
                    *host<uint32_t>(uva(MAIN_OUT)) = KCAP_INVALID;
                    ASSERT_EQ(err(call_as(main_, KOS_SYS_NOTIFY_CREATE, uva(MAIN_OUT))), 0);
                    note_ = *host<uint32_t>(uva(MAIN_OUT));
                    IrqLock lock;
                    obj_ = cap_lookup(main_, note_)->obj;
                }

                Notification* note() const
                {
                    IrqLock lock;
                    return kernel().notifies.resolve(obj_);
                }

                uint8_t note_refs() const
                {
                    IrqLock lock;
                    return kernel().notifies.refs()[kernel().notifies.live_index(obj_)];
                }

                uint32_t pending() const
                {
                    IrqLock lock;
                    return note()->pending;
                }

                // KOS_SYS_NOTIFY_WAIT on BIT, the bits answered at EDIT_OUT.
                int32_t wait(uint32_t* bits)
                {
                    *host<uint32_t>(uva(EDIT_OUT)) = 0;
                    int32_t const rc = err(call_as(main_, KOS_SYS_NOTIFY_WAIT, note_, BIT,
                                                   KOS_TIMEOUT_NONE, uva(EDIT_OUT)));
                    *bits = *host<uint32_t>(uva(EDIT_OUT));
                    return rc;
                }

                uint32_t note_ = KCAP_INVALID;
                int obj_ = -1;
            };

            TEST_F(OutDeliver, a_spawn_whose_handle_is_refused_leaves_no_thread)
            {
                // The child holds a copy of the notification, so the refusal has a reference
                // to give back.
                uintptr_t const p = spawn_params(MAIN_PARAMS, 0, 0, 0);
                uintptr_t const caps = uva(MAIN_PARAMS, CAPS_AT);
                *host<kos_cap_grant>(caps) = {note_, KOS_CAP_SIGNAL};
                params(p)->caps = reinterpret_cast<kos_cap_grant const*>(caps);
                params(p)->cap_count = 1;
                uint32_t const members = task_member_count(task_);
                uint8_t const refs = note_refs();
                size_t const frames = host_pool_used();

                refuse_acquire(uva(EDIT_OUT), 1);
                kos_thread_t child = KOS_THREAD_NONE;
                EXPECT_EQ(spawn(main_, p, EDIT_OUT, &child), -KOS_EFAULT);
                EXPECT_EQ(child, KOS_THREAD_NONE);
                EXPECT_EQ(task_member_count(task_), members) << "the unnamed child joined the task";
                EXPECT_EQ(note_refs(), refs) << "the unnamed child kept its grant's reference";
                EXPECT_EQ(host_pool_used(), frames) << "the unnamed child kept its stack";

                ASSERT_EQ(spawn(main_, p, EDIT_OUT, &child), 0);
                EXPECT_NE(child, KOS_THREAD_NONE);
                EXPECT_EQ(task_member_count(task_), members + 1u);
            }

            TEST_F(OutDeliver, a_create_whose_handle_is_refused_ends_its_task)
            {
                (void)program_for(main_);
                uint32_t const holds = kernel().task_holds;

                refuse_acquire(uva(EDIT_OUT), 1);
                kos_task_t t = KOS_TASK_NONE;
                EXPECT_EQ(create_task(main_, 0, 0, 0, &t, {}, MAIN_PARAMS, EDIT_OUT), -KOS_EFAULT);
                EXPECT_EQ(t, KOS_TASK_NONE);
                EXPECT_EQ(kernel().task_holds, holds) << "the unnamed task kept its creator's hold";
                Thread const* started = nullptr;
                for (Thread const& th : kernel().threads.all())
                {
                    if (th.task_role == TASK_ROLE_ENTRY and th.task != task_
                        and th.state != ThreadState::EXITED
                        and th.cancel_kind != CANCEL_SLAY)
                    {
                        started = &th;
                    }
                }
                EXPECT_EQ(started, nullptr) << "the unnamed task's entry runs";

                ASSERT_EQ(create_task(main_, 0, 0, 0, &t, {}, MAIN_PARAMS, EDIT_OUT), 0);
                EXPECT_NE(t, KOS_TASK_NONE);
                EXPECT_EQ(kernel().task_holds, holds + 1u);
            }

            TEST_F(OutDeliver, a_wait_whose_bits_are_refused_leaves_them_pending)
            {
                ASSERT_EQ(err(call_as(main_, KOS_SYS_NOTIFY_BIND, note_)), 0);
                ASSERT_TRUE(notify_raise(note(), BIT));

                refuse_acquire(uva(EDIT_OUT), 1);
                uint32_t bits = 0;
                EXPECT_EQ(wait(&bits), -KOS_EFAULT);
                EXPECT_EQ(bits, 0u);
                EXPECT_EQ(pending(), BIT) << "the refused wait consumed its bits";

                EXPECT_EQ(wait(&bits), 0);
                EXPECT_EQ(bits, BIT);
                EXPECT_EQ(pending(), 0u);
            }

            // The page the parked wait's out word sits in, as the hook leaves it.
            struct Turned
            {
                uint32_t rights;
                enum arch_map_memtype type;
            };
            Turned g_turned = {};
            uintptr_t g_turned_va = 0;

            void turn_then_raise(Thread* parked)
            {
                parked->wait_result = 0;
                EXPECT_TRUE(remap_page(parked, g_turned_va, g_turned.rights, g_turned.type));
                (void)notify_raise(g_raised, BIT);
            }

            class OutTurned : public OutDeliver, public ::testing::WithParamInterface<Turned>
            {
            };

            // The out word was in a sibling's writable window when the wait parked; the window
            // was replaced at the same address by one the kernel must not write.
            TEST_P(OutTurned, a_parked_wait_whose_out_word_turned_unwritable_is_refused)
            {
                constexpr uint32_t UNTOUCHED = 0xA5A5A5A5u;
                ASSERT_EQ(err(call_as(main_, KOS_SYS_NOTIFY_BIND, note_)), 0);
                uintptr_t const blk = reserve(main_, G, 0);
                ASSERT_NE(blk, 0u);
                kos_thread_t h = KOS_THREAD_NONE;
                ASSERT_EQ(spawn(main_, spawn_params(MAIN_PARAMS, blk, G, 0), MAIN_OUT, &h), 0);
                Thread* const holder = kernel().threads.resolve(h);
                ASSERT_NE(holder, nullptr);
                uintptr_t const got = uva(MAIN_OUT, sizeof(uint64_t));
                ASSERT_EQ(err(call_as(holder, KOS_SYS_WINDOW_GET, 0, got)), 0);
                uintptr_t const out = host<kos_window>(got)->base + sizeof(uint64_t);
                uint32_t* const word = host<uint32_t>(blk + sizeof(uint64_t));
                *word = UNTOUCHED;

                g_raised = note();
                g_turned = GetParam();
                g_turned_va = out;
                wake_next_park(turn_then_raise);
                int32_t const rc = err(call_as(main_, KOS_SYS_NOTIFY_WAIT, note_, BIT,
                                               KOS_TIMEOUT_NONE, out));
                running() = main_;
                EXPECT_EQ(rc, -KOS_EFAULT);
                EXPECT_EQ(*word, UNTOUCHED) << "the delivery wrote the page";
                EXPECT_EQ(pending(), BIT) << "the refused wait consumed the bits it parked for";
            }

            INSTANTIATE_TEST_SUITE_P(ReadOnlyOrDevice, OutTurned,
                                     ::testing::Values(Turned{ARCH_MAP_R, ARCH_MAP_NORMAL},
                                                       Turned{ARCH_MAP_R | ARCH_MAP_W,
                                                              ARCH_MAP_DEVICE}));

            TEST_F(OutDeliver, a_parked_wait_whose_bits_are_refused_leaves_them_pending)
            {
                ASSERT_EQ(err(call_as(main_, KOS_SYS_NOTIFY_BIND, note_)), 0);
                g_raised = note();
                wake_next_park(raise_bit);

                refuse_acquire(uva(EDIT_OUT), 1);
                uint32_t bits = 0;
                EXPECT_EQ(wait(&bits), -KOS_EFAULT);
                running() = main_;
                EXPECT_EQ(bits, 0u);
                EXPECT_EQ(pending(), BIT) << "the refused wait consumed the bits it parked for";

                EXPECT_EQ(wait(&bits), 0);
                EXPECT_EQ(bits, BIT);
                EXPECT_EQ(pending(), 0u);
            }
        }
    }
}
