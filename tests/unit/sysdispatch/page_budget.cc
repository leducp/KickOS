// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The PAGES budget over the REAL system-call, domain, task and address-space layers at the
// translating posture: what a space's own tables, its data and its stacks cost its task, charged
// before the frames are taken, every space's tables taken from the pool; and what a dead
// incarnation's space a borrower keeps still costs its declaration.

#include "../common/seam_pools.h"

#include <stdint.h>

#include <kickos/image.h>

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
            // Two addresses in 1 GiB spans nothing else in the gate maps: a map at either builds
            // tables of its own.
            constexpr uintptr_t FAR_VA = static_cast<uintptr_t>(0x500000000000ull);
            constexpr uintptr_t FAR_VA_2 = FAR_VA + (static_cast<uintptr_t>(1) << 30);

            class PageBudget : public SysFixture
            {
              protected:
                void SetUp() override
                {
                    host_aspace_tables_from_pool(true);
                    SysFixture::SetUp();
                    decls_ = peer_decls();
                    declare(decls_.decl, PEERS);
                }

                void TearDown() override
                {
                    SysFixture::TearDown();
                    host_aspace_tables_from_pool(false);
                }

                void set_pages(int decl, uint32_t pages)
                {
                    decls_.decl[decl].budgets[KOS_BUDGET_PAGES] = pages;
                }

                static Task* resolve(kos_task_t h)
                {
                    IrqLock lock;
                    return task_resolve(h);
                }

                static uint32_t pages_of(Task const* t)
                {
                    IrqLock lock;
                    return task_usage(t, BudgetKind::PAGES);
                }

                int32_t map_far(uint32_t fcap, uint32_t acap, uintptr_t va)
                {
                    return err(call_as(main_, KOS_SYS_FRAME_MAP, fcap, acap, va_word(va), 0));
                }

                PeerDecls decls_ = {};
            };

            // A map's only allocation is the tables it builds in the space it maps into: they are
            // the space's task's, refused at its budget before a frame is taken.
            TEST_F(PageBudget, a_map_s_new_tables_are_refused_past_the_space_s_budget)
            {
                kos_task_t const h = new_task(main_, 0, 0, 0);
                ASSERT_NE(h, KOS_TASK_NONE);
                Task* const peer = resolve(h);
                ASSERT_NE(peer, nullptr);
                int const decl = task_decl_of(peer);
                ASSERT_GE(decl, 0);
                uint32_t const acap = space_cap(main_, task_domain(peer));
                ASSERT_NE(acap, KCAP_INVALID);
                RunCaps const run = mint(1);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);

                ASSERT_EQ(map_far(run.fcap, acap, FAR_VA), 0);
                uint32_t const used = pages_of(peer);

                set_pages(decl, used + 1u);
                size_t const frames = host_pool_used();
                EXPECT_EQ(map_far(run.fcap, acap, FAR_VA_2), -KOS_EAGAIN)
                    << "a map built tables past its space's page budget";
                EXPECT_EQ(host_pool_used(), frames) << "a refused map kept a frame";
                EXPECT_EQ(pages_of(peer), used);

                set_pages(decl, used + 2u);
                EXPECT_EQ(map_far(run.fcap, acap, FAR_VA_2), 0);
                EXPECT_EQ(pages_of(peer), used + 2u)
                    << "the space's task was not charged its tables";
                EXPECT_EQ(host_pool_used(), frames + 2u);
            }

            // A create's root, tables, data and entry stack: below what it builds, every budget
            // refuses it whole; at it, the create holds exactly its budget.
            TEST_F(PageBudget, a_create_is_refused_below_every_page_it_builds)
            {
                kos_task_t const first = new_task(main_, 0, 0, 0);
                ASSERT_NE(first, KOS_TASK_NONE);
                uint32_t const need = pages_of(resolve(first));
                ASSERT_GT(need, 0u);

                // The next create seats the next declaration.
                for (uint32_t b = 0; b < need; b++)
                {
                    set_pages(1, b);
                    size_t const frames = host_pool_used();
                    kos_task_t h = KOS_TASK_NONE;
                    EXPECT_EQ(create_task(main_, 0, 0, 0, &h), -KOS_EAGAIN)
                        << "a create built " << need << " pages under a budget of " << b;
                    EXPECT_EQ(host_pool_used(), frames) << "a refused create kept a frame";
                    if (h != KOS_TASK_NONE)
                    {
                        break;
                    }
                }
                set_pages(1, need);
                kos_task_t const second = new_task(main_, 0, 0, 0);
                ASSERT_NE(second, KOS_TASK_NONE);
                EXPECT_EQ(task_decl_of(resolve(second)), 1);
                EXPECT_EQ(pages_of(resolve(second)), need);
            }

            // Declarations created straight from their records, a member of each reserving in its
            // space, a block handed from one to the next.
            class RetainedSpace : public SysFixture
            {
              protected:
                void SetUp() override
                {
                    SysFixture::SetUp();
                    decls_ = peer_decls();
                    declare(decls_.decl, PEERS);
                }

                // A create's round as main's call makes it: its data staged and copied outside the
                // lock, then its locked pass.
                static Task* create(size_t decl, uintptr_t base, size_t bytes, Domain* donor)
                {
                    {
                        IrqLock lock;
                        (void)presync_begin();
                        presync_stage_image(decl);
                    }
                    presync_run();
                    Task* t = nullptr;
                    {
                        IrqLock lock;
                        int err = 0;
                        t = task_create_declared(decl, image_of_decl(decl), FIXTURE_TASK_TAG,
                                                 reinterpret_cast<void*>(base), bytes, 0u, donor,
                                                 &err, lock);
                        presync_commit();
                        (void)presync_end();
                    }
                    presync_release(t != nullptr);
                    return t;
                }

                static uintptr_t block_of(Thread* member)
                {
                    return call_as(member, KOS_SYS_RAM_ALLOC, G);
                }

                // `t`'s death: its one member, if any, exits, then its creator's hold ends.
                void end(Task* t, Thread* member)
                {
                    if (member != nullptr)
                    {
                        exit_as(member);
                    }
                    IrqLock lock;
                    task_drop_hold(t, lock);
                }

                static uint32_t charged(size_t decl)
                {
                    IrqLock lock;
                    return task_usage(task_decl_record(decl), BudgetKind::PAGES);
                }

                PeerDecls decls_ = {};
            };

            // A dead donor keeps only its lent block, which charges its declaration until the
            // borrower lets it go.
            TEST_F(RetainedSpace, a_dead_donor_keeps_only_its_lent_block_charged)
            {
                Task* const x = create(0, 0, 0, nullptr);
                ASSERT_NE(x, nullptr);
                Thread* const mx = member(2, MAIN_PRIO, x);
                uintptr_t const bx = block_of(mx);
                ASSERT_NE(bx, 0u);
                ASSERT_NE(block_of(mx), 0u) << "fixture: a second block, lent to nobody";
                Domain* const dx = task_domain(x);
                Task* const y = create(1, bx, G, dx);
                ASSERT_NE(y, nullptr);
                end(x, mx);
                EXPECT_EQ(dx->space, nullptr) << "a dead donor's space outlived its task";
                EXPECT_NE(domain_refcount(dx), 0u) << "fixture: the borrower held nothing";
                EXPECT_EQ(charged(0), 1u)
                    << "a dead donor's declaration was charged other than its lent block";
                EXPECT_TRUE(frame_taken(aspace_frame_of(bx)));
                end(y, nullptr);
                EXPECT_FALSE(frame_taken(aspace_frame_of(bx)))
                    << "the lent block outlived its borrower";
                EXPECT_EQ(charged(0), 0u) << "a block freed with its borrower still charges";
            }

            // The restart of a declaration whose lent block a borrower keeps fits a budget of its
            // own space and that block.
            TEST_F(RetainedSpace, a_restart_beside_a_kept_block_fits_its_space_and_the_block)
            {
                Task* x = create(0, 0, 0, nullptr);
                ASSERT_NE(x, nullptr);
                uint32_t const space = charged(0);
                ASSERT_GT(space, 0u);
                Thread* const mx = member(2, MAIN_PRIO, x);
                uintptr_t const bx = block_of(mx);
                ASSERT_NE(bx, 0u);
                ASSERT_NE(create(1, bx, G, task_domain(x)), nullptr);
                decls_.decl[0].budgets[KOS_BUDGET_PAGES] = space + 1u;
                end(x, mx);
                x = create(0, 0, 0, nullptr);
                ASSERT_NE(x, nullptr)
                    << "a restart was refused by the dead incarnation's space a borrower keeps";
                EXPECT_EQ(charged(0), space + 1u);
            }

            TEST_F(RetainedSpace, a_handoff_passed_on_holds_the_domain_that_reserved_it)
            {
                Task* const x = create(0, 0, 0, nullptr);
                ASSERT_NE(x, nullptr);
                Thread* const mx = member(2, MAIN_PRIO, x);
                uintptr_t const bx = block_of(mx);
                ASSERT_NE(bx, 0u);
                Task* const y = create(1, bx, G, task_domain(x));
                ASSERT_NE(y, nullptr);
                Domain* const dy = task_domain(y);
                Task* const z = create(2, bx, G, dy);
                ASSERT_NE(z, nullptr);
                EXPECT_EQ(task_domain(z)->borrowed_from, task_domain(x));
                end(y, nullptr);
                EXPECT_EQ(domain_refcount(dy), 0u)
                    << "a domain a handoff only passed through outlived its task";
                EXPECT_EQ(charged(1), 0u);
            }

            TEST_F(RetainedSpace, a_dead_borrower_s_kept_space_lets_go_of_its_donor)
            {
                Task* const x = create(0, 0, 0, nullptr);
                ASSERT_NE(x, nullptr);
                // Each above the threads still to run, so the one exiting next is the one running.
                Thread* const mx = member(2, MAIN_PRIO + 1, x);
                uintptr_t const bx = block_of(mx);
                ASSERT_NE(bx, 0u);
                Domain* const dx = task_domain(x);
                Task* const y = create(1, bx, G, dx);
                ASSERT_NE(y, nullptr);
                Domain* const dy = task_domain(y);
                Thread* const my = member(3, MAIN_PRIO + 2, y);
                uintptr_t const by = block_of(my);
                ASSERT_NE(by, 0u);
                Task* const z = create(2, by, G, dy);
                ASSERT_NE(z, nullptr);
                end(y, my);
                ASSERT_NE(domain_refcount(dy), 0u) << "fixture: the borrower held nothing";
                EXPECT_FALSE(aspace_range_handed_off(dy->ranges.at_base(bx)))
                    << "a dead task's space kept the block it borrowed";
                EXPECT_EQ(charged(1), 1u);
                end(x, mx);
                EXPECT_EQ(domain_refcount(dx), 0u)
                    << "a dead borrower's kept space held its own donor's space";
            }
        }
    }
}
