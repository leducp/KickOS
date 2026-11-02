// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Two admissions racing one presync, over the REAL system-call, domain, task and address-space
// layers at the translating posture: a call parked at its first window outside the lock, holding
// what it staged, while another call of the same budget runs whole inside that window.

#include "../common/seam_pools.h"

#include <stdint.h>

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
            // The call the first window of the parked call runs, and what it answered.
            Thread* g_racer = nullptr;
            uintptr_t g_race_nr = 0;
            uintptr_t g_race_a0 = 0;
            uintptr_t g_race_a1 = 0;
            uint64_t g_race_rc = 0;
            bool g_raced = false;

            void race_at_first_window(uint32_t ordinal)
            {
                if (ordinal != 1u)
                {
                    return;
                }
                g_race_rc = call_as(g_racer, g_race_nr, g_race_a0, g_race_a1);
                g_raced = true;
            }

            // The create parked at the first window its image copy opens, the declaration
            // record it copies for, what that record was charged then, and the racing create.
            Thread* g_parked = nullptr;
            Task const* g_staged_for = nullptr;
            uint32_t g_charged_while_staged = 0;
            int32_t (*g_race_create)() = nullptr;
            int32_t g_race_create_rc = 0;

            void race_at_image_stage(uint32_t)
            {
                Kernel& k = kernel();
                PresyncRecord const& r = k.presync()[k.threads.index_of(g_parked)];
                if (g_raced or r.stage != PresyncStage::IMAGE)
                {
                    return;
                }
                {
                    IrqLock lock;
                    g_charged_while_staged = task_usage(g_staged_for, BudgetKind::PAGES);
                }
                g_race_create_rc = g_race_create();
                g_raced = true;
            }

            class BudgetRace : public SysFixture
            {
              protected:
                void SetUp() override
                {
                    SysFixture::SetUp();
                    decls_ = peer_decls();
                    declare(decls_.decl, PEERS);
                    // Seated before the create, so the entry thread takes another slot.
                    first_ = member(2, MAIN_PRIO, task_);
                    second_ = member(3, MAIN_PRIO, task_);
                    // Above the tasks it creates, so seating one leaves it the running thread.
                    creator_ = member(4, MAIN_PRIO + 1, task_);
                    kos_task_t const h = new_task(main_, 0, 0, 0);
                    ASSERT_NE(h, KOS_TASK_NONE);
                    {
                        IrqLock lock;
                        peer_ = task_resolve(h);
                    }
                    ASSERT_NE(peer_, nullptr);
                    decl_ = task_decl_of(peer_);
                    ASSERT_GE(decl_, 0);
                    join_task(first_, peer_);
                    join_task(second_, peer_);
                    g_raced = false;
                    g_race_rc = 0;
                }

                void set_budget(kos_budget_kind kind, uint32_t n)
                {
                    decls_.decl[decl_].budgets[kind] = n;
                }

                // `parked`'s call `nr`, its first window running `racer`'s call `race_nr`.
                uint64_t race(Thread* parked, uintptr_t nr, uintptr_t a0, uintptr_t a1,
                              Thread* racer, uintptr_t race_nr, uintptr_t ra0, uintptr_t ra1)
                {
                    g_racer = racer;
                    g_race_nr = race_nr;
                    g_race_a0 = ra0;
                    g_race_a1 = ra1;
                    on_window(race_at_first_window);
                    uint64_t const rc = call_as(parked, nr, a0, a1);
                    on_window(nullptr);
                    return rc;
                }

                void set_budget(int decl, kos_budget_kind kind, uint32_t n)
                {
                    decls_.decl[decl].budgets[kind] = n;
                }

                static Task* resolve(kos_task_t h)
                {
                    IrqLock lock;
                    return task_resolve(h);
                }

                static BudgetRace* g_self;

                static int32_t creator_creates()
                {
                    kos_task_t h = KOS_TASK_NONE;
                    int32_t const rc = g_self->create_task(g_self->creator_, 0, 0, 0, &h, {},
                                                           EDIT_PARAMS, EDIT_OUT);
                    g_self->raced_task_ = h;
                    return rc;
                }

                PeerDecls decls_ = {};
                Thread* first_ = nullptr;
                Thread* second_ = nullptr;
                Thread* creator_ = nullptr;
                kos_task_t raced_task_ = KOS_TASK_NONE;
                Task* peer_ = nullptr;
                int decl_ = -1;
            };

            BudgetRace* BudgetRace::g_self = nullptr;

            TEST_F(BudgetRace, a_reservation_staged_outside_the_lock_holds_its_block)
            {
                set_budget(KOS_BUDGET_RAM_BLOCKS, 1);
                uint64_t const va =
                    race(first_, KOS_SYS_RAM_ALLOC, G, 0, second_, KOS_SYS_RAM_ALLOC, G, 0);
                ASSERT_TRUE(g_raced) << "fixture: the parked call opened no window";
                EXPECT_NE(va, 0u) << "the parked reservation was refused";
                EXPECT_EQ(g_race_rc, 0u)
                    << "a second block passed a budget of one while the first was staged";
            }

            TEST_F(BudgetRace, a_reservation_staged_outside_the_lock_holds_its_bytes)
            {
                set_budget(KOS_BUDGET_RAM_BYTES, static_cast<uint32_t>(G));
                uint64_t const va =
                    race(first_, KOS_SYS_RAM_ALLOC, G, 0, second_, KOS_SYS_RAM_ALLOC, G, 0);
                ASSERT_TRUE(g_raced) << "fixture: the parked call opened no window";
                EXPECT_NE(va, 0u) << "the parked reservation was refused";
                EXPECT_EQ(g_race_rc, 0u)
                    << "a second page passed a budget of one while the first was staged";
            }

            // A frame run's stage is no reservation: it holds pages, not a block.
            TEST_F(BudgetRace, a_frame_run_staged_outside_the_lock_holds_no_block)
            {
                uintptr_t const word = reserve(first_, G, 0);
                ASSERT_NE(word, 0u);
                set_budget(KOS_BUDGET_RAM_BLOCKS, 2);
                *host<uint32_t>(word) = KCAP_INVALID;
                uint64_t const rc = race(first_, KOS_SYS_FRAME_CREATE, G, word, second_,
                                         KOS_SYS_RAM_ALLOC, G, 0);
                ASSERT_TRUE(g_raced) << "fixture: the parked call opened no window";
                EXPECT_EQ(err(rc), 0);
                EXPECT_NE(g_race_rc, 0u) << "a staged frame run was charged as a reservation";
            }

            // The copy of a create's data is its declaration's before a frame is taken.
            TEST_F(BudgetRace, a_create_past_its_declaration_s_pages_takes_no_frame)
            {
                set_budget(decl_ + 1, KOS_BUDGET_PAGES, 0);
                host_pool_reset_counters();
                kos_task_t h = KOS_TASK_NONE;
                EXPECT_EQ(create_task(main_, 0, 0, 0, &h), -KOS_EAGAIN);
                EXPECT_EQ(host_pool_attempts(), 0u)
                    << "a create past its declaration's budget took frames before its refusal";
            }

            // A create parked in the copy of its data holds the declaration it copies for, and
            // the copy's pages charge that declaration; a racing create seats another one.
            TEST_F(BudgetRace, a_create_copying_its_data_holds_its_declaration_and_its_pages)
            {
                ASSERT_NE(program_for(creator_), KCAP_INVALID);
                g_self = this;
                g_parked = main_;
                g_staged_for = task_decl_record(static_cast<size_t>(decl_ + 1));
                ASSERT_NE(g_staged_for, nullptr);
                g_race_create = creator_creates;
                on_window(race_at_image_stage);
                kos_task_t h = KOS_TASK_NONE;
                int32_t const rc = create_task(main_, 0, 0, 0, &h);
                on_window(nullptr);
                ASSERT_TRUE(g_raced) << "fixture: the create opened no window with its copy staged";
                ASSERT_EQ(rc, 0);
                ASSERT_EQ(g_race_create_rc, 0);
                EXPECT_EQ(g_charged_while_staged, SYS_IMAGE_BYTES / G)
                    << "the staged copy charged no declaration";
                EXPECT_EQ(task_decl_of(resolve(h)), decl_ + 1)
                    << "the parked create lost the declaration it copied for";
                EXPECT_EQ(task_decl_of(resolve(raced_task_)), decl_ + 2)
                    << "the racing create seated a declaration another create was copying for";
            }
        }
    }
}
