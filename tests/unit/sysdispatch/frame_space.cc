// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Frame runs and the address-space capability over the REAL capability, domain, task and
// address-space layers at the translating posture: the mint and its budget, a run's holders,
// what a space capability holds, what a space still maps once its task is gone, the memory
// counts, and a thread's stack at its exit.

#include <kickos/slotpool.h>

#include <setjmp.h>
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
            constexpr uintptr_t RUN_VA = static_cast<uintptr_t>(0x500000000000ull);

            class SpaceRetire : public SysFixture
            {
              protected:
                // A task main created, with one member at main's priority, and main's
                // capability over its space.
                void SetUp() override
                {
                    SysFixture::SetUp();
                    spaces_ = domain_spaces_held();
                    handle_ = new_task(main_, 0, 0, 0);
                    ASSERT_NE(handle_, KOS_TASK_NONE);
                    kos_thread_t m = KOS_THREAD_NONE;
                    uintptr_t const p = spawn_params(MAIN_PARAMS, 0, 0, 0, handle_);
                    ASSERT_EQ(spawn(main_, p, MAIN_OUT, &m), 0);
                    member_ = kernel().threads.resolve(m);
                    {
                        IrqLock lock;
                        child_ = task_resolve(handle_);
                    }
                    ASSERT_NE(child_, nullptr);
                    d_ = task_domain(child_);
                    acap_ = space_cap(main_, d_);
                    ASSERT_NE(acap_, KCAP_INVALID);
                }

                // A thread of main's task spawned with `cap` delegated to it.
                Thread* delegate_by_spawn(uint32_t cap)
                {
                    uintptr_t const p = spawn_params(MAIN_PARAMS, 0, 0, 0, KOS_TASK_NONE);
                    *host<kos_cap_grant>(uva(MAIN_PARAMS, CAPS_AT)) = {cap, KOS_CAP_TRANSFER};
                    params(p)->caps =
                        reinterpret_cast<kos_cap_grant const*>(uva(MAIN_PARAMS, CAPS_AT));
                    params(p)->cap_count = 1;
                    kos_thread_t t = KOS_THREAD_NONE;
                    if (spawn(main_, p, MAIN_OUT, &t) != 0)
                    {
                        return nullptr;
                    }
                    return kernel().threads.resolve(t);
                }

                void end()
                {
                    IrqLock lock;
                    task_stop(child_, lock);
                }

                int32_t map(RunCaps const& run)
                {
                    return err(call_as(main_, KOS_SYS_FRAME_MAP, run.fcap, acap_,
                                       va_word(RUN_VA), 0));
                }

                int32_t unmap(RunCaps const& run)
                {
                    return err(call_as(main_, KOS_SYS_FRAME_UNMAP, run.fcap, acap_, RUN_VA));
                }

                size_t spaces_ = 0;
                kos_task_t handle_ = KOS_TASK_NONE;
                Task* child_ = nullptr;
                Thread* member_ = nullptr;
                Domain* d_ = nullptr;
                uint32_t acap_ = KCAP_INVALID;
            };

            TEST_F(SpaceRetire, a_space_capability_holds_no_reference)
            {
                uint16_t const held = d_->refcount;
                ASSERT_NE(delegate_by_spawn(acap_), nullptr);
                EXPECT_EQ(d_->refcount, held) << "the delegated copy took a hold on its space";

                ASSERT_EQ(call_as(main_, KOS_SYS_TASK_KILL, handle_), 0u);
                exit_as(member_);
                EXPECT_EQ(d_->space, nullptr) << "a space capability kept a dead task's space";
                EXPECT_EQ(domain_spaces_held(), spaces_);
                uint16_t const dead = d_->refcount;
                EXPECT_EQ(close(main_, acap_), 0);
                EXPECT_EQ(d_->refcount, dead) << "closing a stale capability dropped a hold";
                EXPECT_EQ(domain_spaces_held(), spaces_);
            }

            TEST_F(SpaceRetire, a_task_life_moves_its_space_s_generation_once)
            {
                uint16_t const born = d_->generation;
                end();
                exit_as(member_);
                {
                    IrqLock lock;
                    task_drop_hold(child_, lock);
                }
                EXPECT_EQ(static_cast<uint16_t>(d_->generation - born), 1u)
                    << "an end, a death and a free moved the generation more than once";
            }

            TEST_F(SpaceRetire, it_resolves_stale_from_the_task_s_end)
            {
                RunCaps const run = mint(1);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                ASSERT_EQ(map(run), 0) << "fixture: the live space refused the map";
                ASSERT_EQ(unmap(run), 0);

                end();
                EXPECT_EQ(map(run), -KOS_EBADF) << "the space was named past its task's end";
                EXPECT_EQ(unmap(run), -KOS_EBADF);

                exit_as(member_);
                ASSERT_NE(d_->space, nullptr) << "fixture: the creator hold kept no space";
                EXPECT_EQ(map(run), -KOS_EBADF) << "the space was named past its task's death";
                EXPECT_EQ(unmap(run), -KOS_EBADF);
                EXPECT_EQ(d_->ranges.at_base(RUN_VA), nullptr);
            }

            TEST_F(SpaceRetire,
                   a_run_mapped_into_a_dead_space_is_unmapped_at_its_death_and_its_reference_returned)
            {
                RunCaps const run = mint(2);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                ASSERT_EQ(map(run), 0);
                EXPECT_EQ(run_refs(run.obj), 2u);

                exit_as(member_);
                ASSERT_NE(d_->space, nullptr) << "fixture: the creator hold kept no space";
                EXPECT_EQ(run_refs(run.obj), 1u) << "the dead space still holds the run";
                EXPECT_EQ(d_->ranges.at_base(RUN_VA), nullptr);
                EXPECT_EQ(arch_aspace_frame_at(d_->space, RUN_VA), 0u)
                    << "the dead space still maps the run";
                ASSERT_EQ(close(main_, run.fcap), 0);
                EXPECT_FALSE(frame_taken(run.base))
                    << "the run's frames stayed with a space whose task is dead";
                EXPECT_FALSE(frame_taken(run.base + G));
            }

            Thread* g_slay_victim = nullptr;
            jmp_buf g_slay_at;

            void slay_in_window(uint32_t)
            {
                if (running() == g_slay_victim)
                {
                    longjmp(g_slay_at, 1);
                }
            }

            class FrameMint : public SysFixture
            {
              protected:
                static int live_runs()
                {
                    int n = 0;
                    for (uint8_t const r : kernel().frame_run_refs)
                    {
                        if (r != 0)
                        {
                            n++;
                        }
                    }
                    return n;
                }

                static bool admits(Task const* t)
                {
                    IrqLock lock;
                    return frame_run_admit(t);
                }
            };

            class FrameRunBudget : public FrameMint
            {
            };

            class FrameRunHold : public FrameMint
            {
            };

            class SpaceSelf : public FrameMint
            {
            };

            class MemCount : public FrameMint
            {
              protected:
                int32_t count(uint32_t which, uint32_t* out)
                {
                    *host<uint32_t>(uva(MAIN_OUT)) = 0xDEADu;
                    int32_t const rc = err(call_as(main_, KOS_SYS_MEM_COUNT, which, uva(MAIN_OUT)));
                    *out = *host<uint32_t>(uva(MAIN_OUT));
                    return rc;
                }

                uint32_t count(uint32_t which)
                {
                    uint32_t out = 0;
                    EXPECT_EQ(count(which, &out), 0);
                    return out;
                }
            };

            TEST_F(FrameMint, two_live_runs_never_share_a_base)
            {
                uint32_t a = KCAP_INVALID;
                uint32_t b = KCAP_INVALID;
                ASSERT_EQ(create(2 * G, &a), 0);
                ASSERT_EQ(create(2 * G, &b), 0);
                FrameRun const* const ra = run_of(a);
                FrameRun const* const rb = run_of(b);
                ASSERT_NE(ra, nullptr);
                ASSERT_NE(rb, nullptr);
                EXPECT_EQ(ra->pages, 2u);
                EXPECT_EQ(rb->pages, 2u);
                bool const apart = ra->base + 2 * G <= rb->base or rb->base + 2 * G <= ra->base;
                EXPECT_TRUE(apart) << "two live runs share frames";
                EXPECT_TRUE(frame_taken(ra->base));
                EXPECT_TRUE(frame_taken(rb->base));
                EXPECT_EQ(close(main_, a), 0);
                EXPECT_EQ(close(main_, b), 0);
            }

            TEST_F(FrameMint, the_run_is_zero_before_its_capability_resolves)
            {
                constexpr uint32_t PAGES = 3;
                dirty_free_frames();
                uint32_t cap = KCAP_INVALID;
                ASSERT_EQ(create(PAGES * G - 1u, &cap), 0);
                FrameRun const* const r = run_of(cap);
                ASSERT_NE(r, nullptr);
                ASSERT_EQ(r->pages, PAGES) << "bytes are not rounded up to whole pages";
                unsigned char const* const bytes = host_pool_bytes(r->base, PAGES);
                size_t dirty = 0;
                for (size_t i = 0; i < PAGES * G; i++)
                {
                    if (bytes[i] != 0)
                    {
                        dirty++;
                    }
                }
                EXPECT_EQ(dirty, 0u) << "the run reached its capability uncleared";
                EXPECT_GE(g_windows, PAGES) << "cleared with no window per granule";
            }

            TEST_F(FrameMint, an_unfinished_clear_fills_no_run)
            {
                int obj = FRAME_RUN_NONE;
                {
                    IrqLock lock;
                    obj = frame_run_create_empty(KOS_TASK_NONE);
                    ASSERT_NE(obj, FRAME_RUN_NONE);
                    ASSERT_NE(aspace_reserve_stage(domain_ranges(thread_domain(main_)), 2 * G),
                              0u);
                    EXPECT_FALSE(aspace_reserve_run(obj)) << "an uncleared stage filled a run";
                    EXPECT_EQ(kernel().frame_runs.resolve(obj)->pages, 0u);
                }
                presync_run();
                {
                    IrqLock lock;
                    EXPECT_TRUE(aspace_reserve_run(obj));
                    EXPECT_EQ(kernel().frame_runs.resolve(obj)->pages, 2u);
                    frame_run_release(obj);
                }
                presync_release(true);
            }

            enum class Short
            {
                NOTHING,
                AUTHORITY,
                RUN_SLOT,
                CAP_SLOT,
                OUT_WORD
            };

            struct Refusal
            {
                char const* name;
                Short shortage;
                size_t bytes;
                int32_t rc;
                bool frame_attempted;
            };

            class FrameMintRefusal : public FrameMint,
                                     public ::testing::WithParamInterface<Refusal>
            {
              protected:
                void make_short(Short what)
                {
                    switch (what)
                    {
                        case Short::NOTHING:
                        {
                            break;
                        }
                        case Short::AUTHORITY:
                        {
                            cap_seat_authority(main_, 0);
                            break;
                        }
                        case Short::RUN_SLOT:
                        {
                            constexpr int UNCHARGED = 2;
                            static_assert(KICKOS_MAX_FRAME_RUNS - UNCHARGED
                                              < KICKOS_TASK_FRAME_RUN_BUDGET,
                                          "the pool must fill before main's budget does");
                            {
                                IrqLock lock;
                                for (int i = 0; i < UNCHARGED; i++)
                                {
                                    ASSERT_NE(frame_run_create_empty(KOS_TASK_NONE),
                                              FRAME_RUN_NONE);
                                }
                            }
                            uint32_t cap = KCAP_INVALID;
                            for (int i = UNCHARGED; i < KICKOS_MAX_FRAME_RUNS; i++)
                            {
                                ASSERT_EQ(create(G, &cap), 0);
                            }
                            break;
                        }
                        case Short::CAP_SLOT:
                        {
                            uint32_t filler = KCAP_INVALID;
                            while (space_self(&filler) == 0)
                            {
                            }
                            break;
                        }
                        case Short::OUT_WORD:
                        {
                            refuse_acquire(uva(MAIN_OUT), 1);
                            break;
                        }
                    }
                }
            };

            TEST_P(FrameMintRefusal, a_refused_mint_leaves_nothing)
            {
                Refusal const& row = GetParam();
                ASSERT_NO_FATAL_FAILURE(make_short(row.shortage));
                host_pool_reset_counters();
                int const runs = live_runs();
                size_t const used = host_pool_used();
                uint32_t cap = 0;
                EXPECT_EQ(create(row.bytes, &cap), row.rc);
                EXPECT_EQ(cap, KCAP_INVALID) << "a refused mint wrote its out word";
                EXPECT_EQ(live_runs(), runs) << "a refused mint stayed charged";
                EXPECT_EQ(host_pool_used(), used) << "a refused mint kept frames";
                if (not row.frame_attempted)
                {
                    EXPECT_EQ(host_pool_attempts(), 0u) << "a frame was taken before the refusal";
                }
            }

            Refusal const REFUSALS[] = {
                {"without_auth_memory_is_eperm", Short::AUTHORITY, G, -KOS_EPERM, false},
                {"bytes_zero_is_einval", Short::NOTHING, 0, -KOS_EINVAL, false},
                {"bytes_past_any_size_are_einval", Short::NOTHING, SIZE_MAX, -KOS_EINVAL, false},
                {"bytes_past_a_run_are_einval", Short::NOTHING, (VR_MAX_PAGES + 1u) * G,
                 -KOS_EINVAL, false},
                {"a_short_pool_is_enomem", Short::NOTHING, (HOST_POOL_FRAMES + 1u) * G,
                 -KOS_ENOMEM, true},
                {"a_full_run_pool_is_enomem", Short::RUN_SLOT, G, -KOS_ENOMEM, false},
                {"a_full_cap_table_is_emfile_before_any_frame_is_taken", Short::CAP_SLOT, G,
                 -KOS_EMFILE, false},
                {"a_mint_whose_out_word_went_away_is_efault", Short::OUT_WORD, 2 * G, -KOS_EFAULT,
                 true},
            };

            INSTANTIATE_TEST_SUITE_P(Short, FrameMintRefusal, ::testing::ValuesIn(REFUSALS),
                                     [](::testing::TestParamInfo<Refusal> const& p)
                                     {
                                         return std::string(p.param.name);
                                     });

            TEST_F(FrameMint, an_unfilled_run_does_not_resolve)
            {
                uint32_t cap = KCAP_INVALID;
                {
                    IrqLock lock;
                    int const obj = frame_run_create_empty(KOS_TASK_NONE);
                    ASSERT_NE(obj, FRAME_RUN_NONE);
                    ASSERT_EQ(cap_install(main_, obj, CapType::CAP_FRAME, CAP_TRANSFER, &cap), 0);
                }
                int err = 0;
                EXPECT_EQ(run_of(main_, cap, &err), nullptr) << "a run naming no frame resolved";
                EXPECT_EQ(err, KOS_EBADF);
                EXPECT_EQ(close(main_, cap), 0);
            }

            TEST_F(FrameMint, a_minter_slain_while_its_run_clears_leaves_nothing)
            {
                int const runs = live_runs();
                size_t const used = host_pool_used();
                uintptr_t const p = spawn_params(EDIT_PARAMS, 0, 0, 0, KOS_TASK_NONE);
                params(p)->authority = KOS_AUTH_MEMORY | KOS_AUTH_TASKS;
                kos_thread_t child = KOS_THREAD_NONE;
                ASSERT_EQ(spawn(main_, p, EDIT_OUT, &child), 0);
                g_slay_victim = kernel().threads.resolve(child);
                sched::yield();
                ASSERT_EQ(running(), g_slay_victim);
                sys_seam_reset();
                on_window(slay_in_window);
                if (setjmp(g_slay_at) == 0)
                {
                    (void)frame_create_call(3 * G, uva(EDIT_OUT));
                    FAIL() << "the mint opened no window";
                }
                on_window(nullptr);
                running() = g_slay_victim;
                ASSERT_EQ(live_runs(), runs + 1) << "fixture: slain before its run was seated";
                run_exit(KOS_EXIT_CANCELLED);
                running() = main_;
                EXPECT_EQ(live_runs(), runs) << "a slain mint left its run";
                EXPECT_EQ(host_pool_used(), used) << "a slain mint kept frames";
                EXPECT_TRUE(admits(task_));
            }

            TEST_F(FrameRunHold, a_map_at_address_0_answers_the_run_s_own_address)
            {
                uint32_t fcap = KCAP_INVALID;
                uint32_t acap = KCAP_INVALID;
                ASSERT_EQ(create(G, &fcap), 0);
                ASSERT_EQ(space_self(&acap), 0);
                int const obj = obj_of(main_, fcap);
                uintptr_t const at = va_word(0);
                ASSERT_EQ(call_as(main_, KOS_SYS_FRAME_MAP, fcap, acap, at, 0), 0u);
                EXPECT_EQ(*host<uintptr_t>(at), aspace_user_va(run_of(fcap)->base))
                    << "the kernel's choice of address was not handed back";
                EXPECT_EQ(run_refs(obj), 2u);
            }

            TEST_F(FrameRunHold, the_address_word_is_checked_as_an_out_word_and_maps_nothing)
            {
                uint32_t fcap = KCAP_INVALID;
                uint32_t acap = KCAP_INVALID;
                ASSERT_EQ(create(G, &fcap), 0);
                ASSERT_EQ(space_self(&acap), 0);
                int const obj = obj_of(main_, fcap);
                EXPECT_EQ(err(call_as(main_, KOS_SYS_FRAME_MAP, fcap, acap, 0, 0)), -KOS_EINVAL);
                uintptr_t const half = va_word(0) + sizeof(uint32_t);
                EXPECT_EQ(err(call_as(main_, KOS_SYS_FRAME_MAP, fcap, acap, half, 0)), -KOS_EINVAL)
                    << "a word off its own alignment was read";
                uintptr_t const at = va_word(0);
                refuse_acquire(at, 1);
                EXPECT_EQ(err(call_as(main_, KOS_SYS_FRAME_MAP, fcap, acap, at, 0)), -KOS_EFAULT);
                EXPECT_EQ(run_refs(obj), 1u) << "a refused map kept a reference";
            }

            TEST_F(FrameRunHold, a_map_whose_address_cannot_be_delivered_maps_nothing)
            {
                uint32_t fcap = KCAP_INVALID;
                uint32_t acap = KCAP_INVALID;
                ASSERT_EQ(create(G, &fcap), 0);
                ASSERT_EQ(space_self(&acap), 0);
                int const obj = obj_of(main_, fcap);
                VirtualRanges const* const ranges = domain_ranges(thread_domain(main_));
                size_t const before = ranges->count();
                uintptr_t const at = va_word(0);
                refuse_acquire(at, 1, 1);
                EXPECT_EQ(err(call_as(main_, KOS_SYS_FRAME_MAP, fcap, acap, at, 0)), -KOS_EFAULT);
                EXPECT_EQ(run_refs(obj), 1u) << "an undeliverable map kept its reference";
                EXPECT_EQ(ranges->count(), before) << "an undeliverable map kept its range";
            }

            TEST_F(FrameRunHold, without_auth_memory_a_bad_address_word_is_eperm)
            {
                uint32_t fcap = KCAP_INVALID;
                uint32_t acap = KCAP_INVALID;
                ASSERT_EQ(create(G, &fcap), 0);
                ASSERT_EQ(space_self(&acap), 0);
                cap_seat_authority(main_, 0);
                EXPECT_EQ(err(call_as(main_, KOS_SYS_FRAME_MAP, fcap, acap, 0, 0)), -KOS_EPERM)
                    << "the address word was judged ahead of the authority";
            }

            TEST_F(FrameRunHold, a_mapping_is_a_holder_and_a_refused_map_returns_its_reference)
            {
                uint32_t fcap = KCAP_INVALID;
                uint32_t acap = KCAP_INVALID;
                ASSERT_EQ(create(2 * G, &fcap), 0);
                ASSERT_EQ(space_self(&acap), 0);
                int const obj = obj_of(main_, fcap);
                arch_phys_addr_t const base = run_of(fcap)->base;
                ASSERT_EQ(call_as(main_, KOS_SYS_FRAME_MAP, fcap, acap, va_word(RUN_VA), 0), 0u);
                EXPECT_EQ(run_refs(obj), 2u);
                EXPECT_EQ(err(call_as(main_, KOS_SYS_FRAME_MAP, fcap, acap, va_word(RUN_VA), 0)),
                          -KOS_ENOMEM);
                EXPECT_EQ(run_refs(obj), 2u) << "a refused map kept a reference";

                EXPECT_EQ(close(main_, fcap), 0);
                EXPECT_EQ(run_refs(obj), 1u);
                EXPECT_TRUE(frame_taken(base)) << "the frames went back under a live mapping";
                EXPECT_EQ(err(call_as(main_, KOS_SYS_FRAME_UNMAP, fcap, acap, RUN_VA)),
                          -KOS_EBADF);
                Domain* const d = thread_domain(main_);
                {
                    IrqLock lock;
                    EXPECT_EQ(aspace_cap_unmap(domain_space(d), domain_ranges_mut(d), RUN_VA, obj),
                              0);
                }
                EXPECT_EQ(arch_aspace_frame_at(domain_space(d), RUN_VA), 0u)
                    << "the unmap left its leaves";
                EXPECT_FALSE(frame_taken(base)) << "the last holder's unmap kept the frames";
            }

            TEST_F(FrameRunHold, the_last_holder_frees_the_frames)
            {
                uint32_t fcap = KCAP_INVALID;
                ASSERT_EQ(create(2 * G, &fcap), 0);
                arch_phys_addr_t const base = run_of(fcap)->base;
                Task* const tk = task(1);
                ASSERT_NE(tk, nullptr);
                Thread* const r = member(2, MAIN_PRIO, tk);
                uint32_t const copy = delegate(r, obj_of(main_, fcap));
                ASSERT_NE(copy, KCAP_INVALID);
                EXPECT_EQ(close(main_, fcap), 0);
                EXPECT_TRUE(frame_taken(base)) << "the frames went back under a live holder";
                EXPECT_EQ(close(r, copy), 0);
                EXPECT_FALSE(frame_taken(base)) << "the last holder's close kept the frames";
                EXPECT_FALSE(frame_taken(base + G));
            }

            class FrameCap : public FrameMint
            {
            };

            class FrameRunRefs : public FrameMint
            {
            };

            class FrameCapAdmit : public FrameMint
            {
            };

            TEST_F(FrameCap, a_freed_run_s_handle_names_nothing_once_its_slot_is_reused)
            {
                uint32_t cap = KCAP_INVALID;
                ASSERT_EQ(create(G, &cap), 0);
                int const old_obj = obj_of(main_, cap);
                int const old_slot = frame_run_slot_of(old_obj);
                ASSERT_GE(old_slot, 0);
                ASSERT_EQ(close(main_, cap), 0);

                // The pool hands a freed slot out last.
                int obj = FRAME_RUN_NONE;
                for (int i = 0; i <= KICKOS_MAX_FRAME_RUNS; i++)
                {
                    ASSERT_EQ(create(G, &cap), 0);
                    obj = obj_of(main_, cap);
                    if (frame_run_slot_of(obj) == old_slot)
                    {
                        break;
                    }
                    ASSERT_EQ(close(main_, cap), 0);
                }
                ASSERT_EQ(frame_run_slot_of(obj), old_slot)
                    << "fixture: the pool never reseated the slot";
                EXPECT_NE(obj, old_obj);
                {
                    IrqLock lock;
                    EXPECT_EQ(frame_run_slot_of(old_obj), -1);
                    EXPECT_FALSE(frame_run_ref(old_obj)) << "a stale handle took a reference";
                    EXPECT_EQ(kernel().frame_run_refs[old_slot], 1u)
                        << "the stale handle moved the new run's count";
                }
                EXPECT_EQ(close(main_, cap), 0);
            }

            TEST_F(FrameRunRefs, a_map_refused_by_the_editor_gives_its_reference_back)
            {
                RunCaps const run = mint(1);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                refuse_map(true);
                EXPECT_EQ(err(call_as(main_, KOS_SYS_FRAME_MAP, run.fcap, run.acap,
                                      va_word(RUN_VA), 0)),
                          -KOS_ENOMEM);
                refuse_map(false);
                EXPECT_EQ(run_refs(run.obj), 1u);
                EXPECT_EQ(domain_ranges(thread_domain(main_))->at_base(RUN_VA), nullptr);
            }

            // No call reaches this arm: no range holds these rights, which are refused after the
            // leaves went in.
            TEST_F(FrameRunRefs, a_map_refused_at_its_grant_gives_its_reference_back)
            {
                constexpr uint32_t RIGHT_NO_RANGE_HOLDS = 0x100u;
                RunCaps const run = mint(1);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                Domain* const d = thread_domain(main_);
                {
                    IrqLock lock;
                    (void)presync_begin();
                    uintptr_t va = RUN_VA;
                    EXPECT_EQ(aspace_cap_map(domain_space(d), domain_ranges_mut(d), &va, run.obj,
                                             run.base, 1, RIGHT_NO_RANGE_HOLDS, ARCH_MAP_NORMAL),
                              -KOS_ENOMEM);
                    (void)presync_end();
                }
                EXPECT_EQ(run_refs(run.obj), 1u);
                EXPECT_EQ(arch_aspace_frame_at(domain_space(d), RUN_VA), 0u)
                    << "the refused map left its leaves";
                EXPECT_EQ(domain_ranges(d)->at_base(RUN_VA), nullptr);
            }

            TEST_F(FrameRunRefs, a_space_torn_down_drops_its_mapping_s_reference)
            {
                RunCaps const run = mint(1);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                ASSERT_EQ(call_as(main_, KOS_SYS_FRAME_MAP, run.fcap, run.acap, va_word(RUN_VA), 0),
                          0u);
                ASSERT_EQ(close(main_, run.fcap), 0);
                ASSERT_TRUE(frame_taken(run.base));
                Domain* const d = thread_domain(main_);
                watch_destroy(RUN_VA);
                {
                    IrqLock lock;
                    aspace_release(d->space, &d->ranges);
                    d->space = nullptr;
                }
                EXPECT_FALSE(g_watched_at_destroy) << "the teardown left the run's leaves";
                EXPECT_EQ(d->ranges.at_base(RUN_VA), nullptr);
                EXPECT_EQ(run_refs(run.obj), 0u)
                    << "the teardown kept the reference its mapping held";
                EXPECT_FALSE(frame_taken(run.base));
            }

            TEST_F(FrameCapAdmit, a_handoff_over_a_mapped_run_is_eperm_and_takes_no_reference)
            {
                RunCaps const run = mint(1);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                ASSERT_EQ(call_as(main_, KOS_SYS_FRAME_MAP, run.fcap, run.acap, va_word(RUN_VA), 0),
                          0u);
                kos_task_t t = KOS_TASK_NONE;
                EXPECT_EQ(create_task(main_, RUN_VA, G, 0, &t), -KOS_EPERM)
                    << "the mapping was handed off as a reservation";
                EXPECT_EQ(t, KOS_TASK_NONE);
                EXPECT_EQ(run_refs(run.obj), 2u);
            }

            class SysStackExit : public SysFixture
            {
            };

            TEST_F(SysStackExit, a_thread_exit_unmaps_its_whole_stack_in_one_call)
            {
                kos_thread_t child = KOS_THREAD_NONE;
                ASSERT_EQ(spawn(main_, spawn_params(MAIN_PARAMS, 0, 0, 0, KOS_TASK_NONE),
                                MAIN_OUT, &child),
                          0);
                Thread* const t = kernel().threads.resolve(child);
                ASSERT_NE(t, nullptr);
                size_t const pages = t->stack_size / G;
                ASSERT_GT(pages, 1u) << "one page cannot tell one unmap from one per page";
                sys_seam_reset();
                exit_as(t);
                EXPECT_EQ(g_unmaps, 1u) << "the exit unmapped its stack in more than one call";
                EXPECT_EQ(g_unmapped_pages, pages) << "the exit left part of its stack mapped";
            }

            TEST_F(FrameRunBudget, the_ceiling_is_eagain)
            {
                uint32_t cap = KCAP_INVALID;
                for (int i = 0; i < KICKOS_TASK_FRAME_RUN_BUDGET; i++)
                {
                    ASSERT_EQ(create(G, &cap), 0);
                }
                int const runs = live_runs();
                ASSERT_LT(runs, KICKOS_MAX_FRAME_RUNS) << "fixture: the pool itself is full";
                EXPECT_EQ(create(G, &cap), -KOS_EAGAIN);
                EXPECT_EQ(live_runs(), runs);
            }

            TEST_F(FrameRunBudget, a_run_mapped_elsewhere_and_closed_still_charges_its_minter)
            {
                Task* const tk = task(1);
                ASSERT_NE(tk, nullptr);
                Domain* const d = task_domain(tk);
                uint32_t fcap = KCAP_INVALID;
                ASSERT_EQ(create(G, &fcap), 0);
                uint32_t const acap = space_cap(main_, d);
                ASSERT_NE(acap, KCAP_INVALID);
                ASSERT_EQ(call_as(main_, KOS_SYS_FRAME_MAP, fcap, acap, va_word(RUN_VA), 0), 0u);
                ASSERT_EQ(close(main_, fcap), 0);
                uint32_t cap = KCAP_INVALID;
                for (int i = 1; i < KICKOS_TASK_FRAME_RUN_BUDGET; i++)
                {
                    ASSERT_EQ(create(G, &cap), 0);
                }
                EXPECT_EQ(create(G, &cap), -KOS_EAGAIN)
                    << "a run mapped in another space escaped its minter's budget";
                EXPECT_TRUE(admits(tk)) << "the space mapping the run was charged for it";
            }

            TEST_F(FrameRunBudget, the_receiver_of_a_delegated_run_is_not_charged)
            {
                Task* const tk = task(1);
                ASSERT_NE(tk, nullptr);
                Thread* const r = member(2, MAIN_PRIO, tk);
                for (int i = 0; i < KICKOS_TASK_FRAME_RUN_BUDGET; i++)
                {
                    uint32_t cap = KCAP_INVALID;
                    ASSERT_EQ(create(G, &cap), 0);
                    ASSERT_NE(delegate(r, obj_of(main_, cap)), KCAP_INVALID);
                    ASSERT_EQ(close(main_, cap), 0);
                }
                EXPECT_FALSE(admits(task_)) << "the minter's charge ended with its capabilities";
                EXPECT_TRUE(admits(tk)) << "the receiver was charged for runs it holds";
            }

            TEST_F(FrameRunBudget, the_charge_ends_at_the_last_holder)
            {
                Task* const tk = task(1);
                ASSERT_NE(tk, nullptr);
                Thread* const r = member(2, MAIN_PRIO, tk);
                uint32_t cap = KCAP_INVALID;
                for (int i = 0; i < KICKOS_TASK_FRAME_RUN_BUDGET; i++)
                {
                    ASSERT_EQ(create(G, &cap), 0);
                }
                uint32_t const copy = delegate(r, obj_of(main_, cap));
                ASSERT_NE(copy, KCAP_INVALID);
                ASSERT_EQ(close(main_, cap), 0);
                EXPECT_FALSE(admits(task_)) << "a run's charge ended before its last holder";
                ASSERT_EQ(close(r, copy), 0);
                EXPECT_TRUE(admits(task_)) << "a freed run still charges its minter";
            }

            TEST_F(FrameRunBudget, a_dead_minter_s_runs_charge_its_successor_nothing)
            {
                Task* const tk = task(1);
                ASSERT_NE(tk, nullptr);
                int const slot = slot_index_of(kernel().tasks, tk);
                Thread* const minter = member(2, MAIN_PRIO, tk);
                uintptr_t const word = reserve(minter, G, 0);
                ASSERT_NE(word, 0u);
                for (int i = 0; i < KICKOS_TASK_FRAME_RUN_BUDGET; i++)
                {
                    uint32_t cap = KCAP_INVALID;
                    ASSERT_EQ(create_as(minter, word, G, &cap), 0);
                    ASSERT_NE(delegate(main_, obj_of(minter, cap)), KCAP_INVALID);
                }
                ASSERT_FALSE(admits(tk)) << "fixture: the minter is not at its budget";
                exit_as(minter);
                {
                    IrqLock lock;
                    task_drop_hold(tk, lock);
                }
                kos_task_t const next = new_task(main_, 0, 0, 0);
                ASSERT_NE(next, KOS_TASK_NONE);
                Task* const successor = task_resolve(next);
                ASSERT_EQ(slot_index_of(kernel().tasks, successor), slot)
                    << "fixture: the successor took another slot";
                EXPECT_TRUE(admits(successor)) << "a dead minter's runs charge its successor";
            }

            TEST_F(SpaceSelf, names_the_caller_s_space_and_holds_no_reference)
            {
                Domain* const d = thread_domain(main_);
                uint16_t const held = d->refcount;
                uint32_t acap = KCAP_INVALID;
                ASSERT_EQ(space_self(&acap), 0);
                EXPECT_EQ(d->refcount, held) << "the capability took a hold on its space";
                int err = -1;
                {
                    IrqLock lock;
                    EXPECT_EQ(cap_resolve_e(main_, acap, CapType::CAP_ASPACE, 0, &err), d);
                }
                EXPECT_EQ(err, 0);
                EXPECT_EQ(close(main_, acap), 0);
                EXPECT_EQ(d->refcount, held);
            }

            TEST_F(SpaceSelf, an_undeliverable_capability_gives_its_slot_back)
            {
                uint32_t last = KCAP_INVALID;
                uint32_t acap = KCAP_INVALID;
                while (space_self(&acap) == 0)
                {
                    last = acap;
                }
                ASSERT_EQ(close(main_, last), 0) << "fixture: the table holds no free slot";
                refuse_acquire(uva(MAIN_OUT), 1);
                EXPECT_EQ(space_self(&acap), -KOS_EFAULT);
                EXPECT_EQ(acap, KCAP_INVALID);
                EXPECT_EQ(space_self(&acap), 0) << "the undelivered capability kept its slot";
            }

            TEST_F(SpaceSelf, no_space_is_einval)
            {
                int derr = 0;
                Task* tk = nullptr;
                {
                    IrqLock lock;
                    tk = task_for(DOM_CALLER_PRIVILEGED, nullptr, 0, nullptr, &derr);
                }
                ASSERT_NE(tk, nullptr);
                Thread* const p = seat_pool(2, MAIN_PRIO);
                p->privileged = true;
                attach_caps(p, CAPS);
                join_task(p, tk);
                running() = p;
                int32_t const rc = aspace_self_call(uva(MAIN_OUT));
                running() = main_;
                EXPECT_EQ(rc, -KOS_EINVAL);
            }

            TEST_F(MemCount, each_id_answers_its_source)
            {
                EXPECT_EQ(count(KOS_MEM_FRAMES_FREE), frame_pool_free());
                EXPECT_EQ(count(KOS_MEM_SPACES_HELD), domain_spaces_held());
                VirtualRanges const* const ranges = domain_ranges(thread_domain(main_));
                EXPECT_EQ(count(KOS_MEM_RANGES_FREE),
                          VirtualRanges::capacity() - ranges->count());
            }

            TEST_F(MemCount, frames_free_falls_by_a_mint_and_returns_at_its_last_holder)
            {
                uint32_t const before = count(KOS_MEM_FRAMES_FREE);
                uint32_t cap = KCAP_INVALID;
                ASSERT_EQ(create(3 * G, &cap), 0);
                EXPECT_EQ(count(KOS_MEM_FRAMES_FREE), before - 3u);
                ASSERT_EQ(close(main_, cap), 0);
                EXPECT_EQ(count(KOS_MEM_FRAMES_FREE), before);
            }

            TEST_F(MemCount, ranges_free_falls_by_a_stack)
            {
                uint32_t const before = count(KOS_MEM_RANGES_FREE);
                kos_thread_t child = KOS_THREAD_NONE;
                ASSERT_EQ(spawn(main_, spawn_params(MAIN_PARAMS, 0, 0, 0, KOS_TASK_NONE),
                                MAIN_OUT, &child),
                          0);
                Thread* const t = kernel().threads.resolve(child);
                ASSERT_NE(t, nullptr);
                EXPECT_LT(count(KOS_MEM_RANGES_FREE), before) << "a stack took no range";
                exit_as(t);
                EXPECT_EQ(count(KOS_MEM_RANGES_FREE), before);
            }

            TEST_F(MemCount, spaces_held_falls_at_the_domain_s_release)
            {
                uint32_t const before = count(KOS_MEM_SPACES_HELD);
                kos_task_t const t = new_task(main_, 0, 0, 0);
                ASSERT_NE(t, KOS_TASK_NONE);
                EXPECT_EQ(count(KOS_MEM_SPACES_HELD), before + 1u);
                ASSERT_EQ(call_as(main_, KOS_SYS_TASK_KILL, t), 0u);
                EXPECT_EQ(count(KOS_MEM_SPACES_HELD), before);
            }

            TEST_F(MemCount, without_auth_memory_is_eperm)
            {
                cap_seat_authority(main_, 0);
                uint32_t out = 0;
                EXPECT_EQ(count(KOS_MEM_FRAMES_FREE, &out), -KOS_EPERM);
                EXPECT_EQ(out, 0xDEADu) << "a refused read wrote its out word";
            }

            TEST_F(MemCount, an_unknown_id_is_einval)
            {
                uint32_t out = 0;
                EXPECT_EQ(count(KOS_MEM_RANGES_FREE + 1u, &out), -KOS_EINVAL);
                EXPECT_EQ(out, 0xDEADu);
            }
        }
    }
}
