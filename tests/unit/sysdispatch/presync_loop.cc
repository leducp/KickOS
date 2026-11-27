// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A system call's work outside the kernel lock, over the REAL kernel/syscall/syscall.cc and
// syscall_thread.cc on the translating posture: the round the dispatch sends a call through
// again when its record drops, what another thread's call completed in an interrupt window
// does to it, what a slain or a refused call leaves behind, and the syncs each mapping call
// owes. The window is the seam's: a case acts there as the thread an interrupt would wake. A copy
// staged for a declaration holds an INSTANCE watch on it until it leaves its stager.

#include <kickos/notify.h>
#include <kickos/start_watch.h>
#include <kickos/sync.h>
#include <kickos/sys/init.h>
#include <kickos/ustack.h>

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <tuple>

#include "stub_seam.h"
#include "sys_fixture.h"

#if not KICKOS_PRESYNC or not KICKOS_ARCH_ALIAS_DCACHE
#error "this gate's posture syncs a cacheable kernel view outside the lock"
#endif

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
            uint32_t watch_spec(uint32_t kind, uint32_t events)
            {
                return (kind << KOS_WATCH_KIND_SHIFT) | events;
            }

            constexpr uint32_t PAGES = 20;
            constexpr uint32_t DROPS = 3;
            constexpr uintptr_t RUN_VA = static_cast<uintptr_t>(0x500000000000ull);
            constexpr uintptr_t RUN_VA2 = RUN_VA + PAGES * G;
            constexpr uint8_t EDITOR_PRIO = 20;
            constexpr uint32_t MAX_KILL_ROUNDS = 8;
            constexpr uintptr_t UNMAPPED = 0x10u;
            constexpr uint32_t UNDEFINED_AUTHORITY = 0x80000000u;
            constexpr kos_cap_t NO_SUCH_CAP = 0x7FFEu;
            constexpr uint16_t PAST_CHILD_TABLE = 0x7FFFu;

            PresyncRecord& record_of(Thread const* t)
            {
                int const i = kernel().threads.index_of(t);
                if (i < 0)
                {
                    printf("FIXTURE FAIL: a thread outside the pool has no presync record\n");
                    exit(1);
                }
                return kernel().presync()[i];
            }

            // presync_drop, which aspace.cc keeps file-local.
            void drop(PresyncRecord& r)
            {
                if (r.live)
                {
                    r.live = false;
                    kernel().presync_live--;
                }
            }

            int live_notifications()
            {
                int n = 0;
                for (uint8_t const r : PoolSpan<uint8_t>{kernel().notifies.refs(), kernel().notifies.count()})
                {
                    if (r != 0)
                    {
                        n++;
                    }
                }
                return n;
            }

            int created_tasks()
            {
                int n = 0;
                for (Task const& t : kernel().tasks())
                {
                    if (t.creator_tag != ThreadPool::KILL_TAG_NONE)
                    {
                        n++;
                    }
                }
                return n;
            }

            Thread* g_main = nullptr;
            Thread* g_editor = nullptr;
            uint32_t g_drops_left = 0;

            // At the first window of each of the main call's rounds.
            void drop_hook(uint32_t)
            {
                PresyncRecord& r = record_of(g_main);
                if (running() == g_main and g_drops_left != 0 and r.live)
                {
                    drop(r);
                    g_drops_left--;
                }
            }

            enum class Edit
            {
                CAP_MAP,
                SELF_GRANT,
                SELF_GRANT_CACHED,
                WINDOW,
                HANDOFF,
                FAULTED_OUT,
                FAILED_SPAWN,
                UNMAP,
                CLOSE,
                READ,
                FLIP
            };

            struct Seen
            {
                bool ran = false;
                bool live_before = false;
                bool live_after = false;
                int32_t rc = -1;
                uint32_t syncs = 0;
            };

            Thread* g_victim = nullptr;
            jmp_buf g_slain;

            void slay_hook(uint32_t)
            {
                if (running() == g_victim)
                {
                    longjmp(g_slain, 1);
                }
            }

            class SysPresync : public SysFixture
            {
              protected:
                void SetUp() override
                {
                    g_case = this;
                    g_drops_left = 0;
                    g_seen = Seen{};
                    SysFixture::SetUp();
                    g_main = main_;
                }

                // Parked on the line until the window wakes it.
                void seat_others() override
                {
                    g_editor = member(2, EDITOR_PRIO, task_);
                    detach_ready(g_editor);
                    seat_blocked(g_editor);
                }

                void edit()
                {
                    PresyncRecord const& main_record = record_of(g_main);
                    switch (edit_)
                    {
                        case Edit::CAP_MAP:
                        {
                            g_seen.rc = err(call_as(g_editor, KOS_SYS_FRAME_MAP, ed_run_.fcap,
                                                    ed_run_.acap, va_word(RUN_VA2, EDIT_OUT), 0));
                            g_seen.live_after = main_record.live;
                            (void)call_as(g_editor, KOS_SYS_FRAME_UNMAP, ed_run_.fcap,
                                          ed_run_.acap, RUN_VA2);
                            break;
                        }
                        case Edit::SELF_GRANT:
                        {
                            g_seen.rc = err(call_as(g_editor, KOS_SYS_MEM_SELF_GRANT, blk_,
                                                    bytes_, KOS_MEM_NOCACHE));
                            g_seen.live_after = main_record.live;
                            break;
                        }
                        case Edit::SELF_GRANT_CACHED:
                        {
                            g_seen.rc = err(call_as(g_editor, KOS_SYS_MEM_SELF_GRANT, blk_,
                                                    bytes_, 0));
                            g_seen.live_after = main_record.live;
                            break;
                        }
                        case Edit::WINDOW:
                        case Edit::FAULTED_OUT:
                        case Edit::FAILED_SPAWN:
                        {
                            kos_task_params* const tp =
                                task_params(EDIT_PARAMS, 0, 0, 0,
                                            PeerWindow{blk_, bytes_, KOS_WINDOW_UNCACHED});
                            if (edit_ == Edit::FAILED_SPAWN)
                            {
                                // A destination past the child's table fails the create after
                                // its window is mapped.
                                uintptr_t const caps = uva(EDIT_PARAMS, CAPS_AT);
                                uintptr_t const dest = uva(EDIT_PARAMS, DEST_AT);
                                *host<kos_cap_grant>(caps) = {ed_run_.acap, KOS_CAP_TRANSFER};
                                *host<uint16_t>(dest) = PAST_CHILD_TABLE;
                                tp->caps = reinterpret_cast<kos_cap_grant const*>(caps);
                                tp->cap_dest = reinterpret_cast<uint16_t const*>(dest);
                                tp->cap_count = 1;
                            }
                            if (edit_ == Edit::FAULTED_OUT)
                            {
                                refuse_acquire(uva(EDIT_OUT), 1);
                            }
                            kos_task_t child = KOS_TASK_NONE;
                            g_seen.rc = create_staged(g_editor, EDIT_PARAMS, EDIT_OUT, &child);
                            g_seen.live_after = main_record.live;
                            break;
                        }
                        case Edit::HANDOFF:
                        {
                            kos_task_t t = KOS_TASK_NONE;
                            g_seen.rc = create_task(g_editor, blk_, bytes_, KOS_MEM_NOCACHE, &t,
                                                    {}, EDIT_PARAMS, EDIT_OUT);
                            g_seen.live_after = main_record.live;
                            break;
                        }
                        case Edit::UNMAP:
                        {
                            g_seen.rc = err(call_as(g_editor, KOS_SYS_FRAME_UNMAP, ed_run_.fcap,
                                                    ed_run_.acap, RUN_VA2));
                            g_seen.live_after = main_record.live;
                            break;
                        }
                        case Edit::CLOSE:
                        {
                            g_seen.rc = err(call_as(g_editor, KOS_SYS_HANDLE_CLOSE, ed_run_.fcap));
                            g_seen.live_after = main_record.live;
                            break;
                        }
                        case Edit::READ:
                        {
                            g_seen.rc = 0;
                            if (call_as(g_editor, KOS_SYS_CPU_CLOCK_HZ) != STUB_CPU_CLOCK_HZ)
                            {
                                g_seen.rc = -1;
                            }
                            g_seen.live_after = main_record.live;
                            break;
                        }
                        case Edit::FLIP:
                        {
                            kos_thread_params* const p = params(uva(MAIN_PARAMS));
                            p->prio = 1;
                            p->window_count = 0;
                            g_seen.rc = 0;
                            g_seen.live_after = main_record.live;
                            break;
                        }
                    }
                }

                static void edit_hook(uint32_t)
                {
                    if (running() != g_main or g_seen.ran)
                    {
                        return;
                    }
                    g_seen.ran = true;
                    g_seen.live_before = record_of(g_main).live;
                    uint32_t const s0 = g_syncs;
                    // The editor preempts main for the edit: a create's pass meets it running.
                    policy_on_ready(g_editor);
                    g_editor->state.to<ThreadState::RUNNING>();
                    g_case->edit();
                    detach_ready(g_editor);
                    seat_blocked(g_editor);
                    g_seen.syncs = g_syncs - s0;
                }

                void arm(Edit what, uintptr_t blk, size_t bytes)
                {
                    edit_ = what;
                    blk_ = blk;
                    bytes_ = bytes;
                    g_seen = Seen{};
                    sys_seam_reset();
                    on_window(edit_hook);
                }

                // Rounds the main call was sent back, from the syncs it made itself.
                uint32_t rounds() const
                {
                    return (g_syncs - g_seen.syncs) / PAGES - 1u;
                }

                static void drop_records()
                {
                    sys_seam_reset();
                    g_drops_left = DROPS;
                    on_window(drop_hook);
                }

                static void expect_rounds_dropped(uint32_t windows_per_round)
                {
                    EXPECT_EQ(g_drops_left, 0u);
                    EXPECT_EQ(g_syncs, (DROPS + 1u) * PAGES) << "a sync per granule per round";
                    EXPECT_EQ(g_windows, (DROPS + 1u) * windows_per_round + DROPS)
                        << "a window per granule synced or copied and one between two rounds";
                    EXPECT_FALSE(record_of(g_main).active);
                }

                void expect_seen(int32_t rc, bool live_after, uint32_t n, char const* what) const
                {
                    EXPECT_TRUE(g_seen.ran and g_seen.live_before);
                    EXPECT_EQ(g_seen.rc, rc) << what;
                    EXPECT_EQ(g_seen.live_after, live_after) << what;
                    EXPECT_EQ(rounds(), n);
                }

                // peer0 started by the editor and slain at once, its entry yet to run its exit.
                Thread* editor_starts_a_dying_peer(Task** record)
                {
                    policy_on_ready(g_editor);
                    g_editor->state.to<ThreadState::RUNNING>();
                    Thread* member = nullptr;
                    kos_task_t t = KOS_TASK_NONE;
                    if (create_task(g_editor, 0, 0, 0, &t, {}, EDIT_PARAMS, EDIT_OUT) == 0)
                    {
                        member = entry_of(t);
                        {
                            IrqLock lock;
                            *record = task_resolve(t);
                        }
                        if (err(call_as(g_editor, KOS_SYS_TASK_SLAY, t, 0)) != 0)
                        {
                            member = nullptr;
                        }
                    }
                    detach_ready(g_editor);
                    seat_blocked(g_editor);
                    running() = g_main;
                    return member;
                }

                // A member of main's task holding AUTH_MEMORY, running.
                Thread* spawn_member()
                {
                    uintptr_t const p = spawn_params(EDIT_PARAMS, 0, 0, 0);
                    params(p)->authority = KOS_AUTH_MEMORY | KOS_AUTH_TASKS;
                    kos_thread_t child = KOS_THREAD_NONE;
                    if (spawn(g_main, p, EDIT_OUT, &child) != 0)
                    {
                        return nullptr;
                    }
                    Thread* const t = kernel().threads.resolve(child);
                    sched::yield();
                    if (running() != t)
                    {
                        return nullptr;
                    }
                    return t;
                }

              public:
                RunCaps share_to(Thread* t, RunCaps const& run)
                {
                    return share(t, run);
                }

                static uint32_t space_cap_to(Thread* holder, Domain* d)
                {
                    return space_cap(holder, d);
                }

                static int close_cap(Thread* t, uint32_t cap)
                {
                    return close(t, cap);
                }

              protected:
                static SysPresync* g_case;
                static Seen g_seen;

                Edit edit_ = Edit::READ;
                uintptr_t blk_ = 0;
                size_t bytes_ = 0;
                RunCaps ed_run_;
            };

            SysPresync* SysPresync::g_case = nullptr;
            Seen SysPresync::g_seen = {};

            TEST_F(SysPresync, a_frame_map_whose_record_drops_syncs_again_each_round)
            {
                RunCaps const run = mint(PAGES);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                drop_records();
                EXPECT_EQ(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, run.acap, va_word(RUN_VA),
                                  KOS_MEM_NOCACHE),
                          0u);
                expect_rounds_dropped(PAGES);
            }

            TEST_F(SysPresync, a_self_grant_whose_record_drops_syncs_again_each_round)
            {
                uintptr_t const blk = reserve_only(PAGES * G);
                ASSERT_NE(blk, 0u);
                drop_records();
                EXPECT_EQ(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, blk, PAGES * G, KOS_MEM_NOCACHE),
                          0u);
                expect_rounds_dropped(PAGES);
            }

            TEST_F(SysPresync, a_task_handed_data_whose_record_drops_syncs_again_each_round)
            {
                uintptr_t const blk = reserve_only(PAGES * G);
                ASSERT_NE(blk, 0u);
                drop_records();
                EXPECT_NE(new_task(g_main, blk, PAGES * G, KOS_MEM_NOCACHE), KOS_TASK_NONE);
                expect_rounds_dropped(PAGES + SYS_IMAGE_BYTES / G);
            }

            TEST_F(SysPresync, a_window_spawn_whose_record_drops_syncs_again_each_round)
            {
                uintptr_t const blk = reserve_only(PAGES * G);
                ASSERT_NE(blk, 0u);
                drop_records();
                kos_thread_t child = KOS_THREAD_NONE;
                EXPECT_EQ(spawn(g_main, window_spawn(blk, PAGES * G), MAIN_OUT, &child), 0);
                EXPECT_NE(child, KOS_THREAD_NONE);
                expect_rounds_dropped(PAGES);
            }

            TEST_F(SysPresync, a_frame_map_completed_in_the_window_sends_a_frame_map_round)
            {
                RunCaps const run = mint(PAGES);
                ed_run_ = share(g_editor, run);
                ASSERT_NE(ed_run_.obj, FRAME_RUN_NONE);
                arm(Edit::CAP_MAP, 0, 0);
                EXPECT_EQ(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, run.acap, va_word(RUN_VA),
                                  KOS_MEM_NOCACHE),
                          0u);
                expect_seen(0, false, 1, "the editor's completed map dropped it");
            }

            TEST_F(SysPresync, a_self_grant_completed_in_the_window_sends_a_handoff_round)
            {
                uintptr_t const blk = reserve_only(PAGES * G);
                ASSERT_NE(blk, 0u);
                arm(Edit::SELF_GRANT, blk, PAGES * G);
                EXPECT_NE(new_task(g_main, blk, PAGES * G, KOS_MEM_NOCACHE), KOS_TASK_NONE);
                expect_seen(0, false, 1, "the editor's completed grant dropped it");
            }

            TEST_F(SysPresync, a_window_spawn_completed_in_the_window_sends_a_handoff_round)
            {
                uintptr_t const blk = reserve_only(PAGES * G);
                ASSERT_NE(blk, 0u);
                arm(Edit::WINDOW, blk, PAGES * G);
                EXPECT_NE(new_task(g_main, blk, PAGES * G, KOS_MEM_NOCACHE), KOS_TASK_NONE);
                expect_seen(0, false, 1, "the editor's completed spawn dropped it");
            }

            TEST_F(SysPresync, a_handoff_completed_in_the_window_sends_a_window_spawn_round)
            {
                uintptr_t const blk = reserve_only(PAGES * G);
                ASSERT_NE(blk, 0u);
                arm(Edit::HANDOFF, blk, PAGES * G);
                kos_thread_t child = KOS_THREAD_NONE;
                EXPECT_EQ(spawn(g_main, window_spawn(blk, PAGES * G), MAIN_OUT, &child), 0);
                expect_seen(0, false, 1, "the editor's completed handoff dropped it");
            }

            TEST_F(SysPresync, a_spawn_whose_handle_is_lost_still_sends_the_call_round)
            {
                uintptr_t const blk = reserve_only(PAGES * G);
                ASSERT_NE(blk, 0u);
                arm(Edit::FAULTED_OUT, blk, PAGES * G);
                EXPECT_NE(new_task(g_main, blk, PAGES * G, KOS_MEM_NOCACHE), KOS_TASK_NONE);
                expect_seen(-KOS_EFAULT, false, 1,
                            "its handle was refused after the window was mapped");
            }

            TEST_F(SysPresync, a_spawn_failing_after_its_window_sends_nothing_round)
            {
                uintptr_t const blk = reserve_only(PAGES * G);
                ASSERT_NE(blk, 0u);
                ed_run_ = share(g_editor, mint(1));
                ASSERT_NE(ed_run_.obj, FRAME_RUN_NONE);
                arm(Edit::FAILED_SPAWN, blk, PAGES * G);
                EXPECT_NE(new_task(g_main, blk, PAGES * G, KOS_MEM_NOCACHE), KOS_TASK_NONE);
                expect_seen(-KOS_EINVAL, true, 0, "the failed spawn dropped it");
            }

            TEST_F(SysPresync, an_unmap_a_close_or_a_read_in_the_window_sends_nothing_round)
            {
                RunCaps const run = mint(PAGES);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                ed_run_ = share(g_editor, run);
                ASSERT_NE(ed_run_.obj, FRAME_RUN_NONE);
                Edit const edits[] = {Edit::UNMAP, Edit::CLOSE, Edit::READ};
                for (Edit const e : edits)
                {
                    // A mapping of the run's own type that stood before the call.
                    (void)call_as(g_main, KOS_SYS_FRAME_UNMAP, run.fcap, run.acap, RUN_VA2);
                    ASSERT_EQ(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, run.acap,
                                      va_word(RUN_VA2), KOS_MEM_NOCACHE),
                              0u);
                    arm(e, 0, 0);
                    EXPECT_EQ(err(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, run.acap,
                                          va_word(RUN_VA), KOS_MEM_NOCACHE)),
                              0);
                    EXPECT_TRUE(g_seen.ran and g_seen.live_before);
                    EXPECT_EQ(g_seen.rc, 0);
                    EXPECT_TRUE(g_seen.live_after);
                    EXPECT_EQ(rounds(), 0u);
                    EXPECT_EQ(call_as(g_main, KOS_SYS_FRAME_UNMAP, run.fcap, run.acap, RUN_VA),
                              0u);
                }
            }

            TEST_F(SysPresync, params_rewritten_in_the_window_change_nothing_the_spawn_does)
            {
                uintptr_t const blk = reserve_only(PAGES * G);
                ASSERT_NE(blk, 0u);
                uintptr_t const p = window_spawn(blk, PAGES * G);
                arm(Edit::FLIP, 0, 0);
                kos_thread_t child = KOS_THREAD_NONE;
                EXPECT_EQ(spawn(g_main, p, MAIN_OUT, &child), 0);
                EXPECT_TRUE(g_seen.ran);
                EXPECT_EQ(rounds(), 0u);
                Thread const* const t = kernel().threads.resolve(child);
                ASSERT_NE(t, nullptr);
                EXPECT_EQ(t->base_prio, MAIN_PRIO) << "the child took the priority first named";
            }

            enum class Slain
            {
                SYNC,
                CLEAR,
                COPY
            };

            class SysPresyncSlain : public SysPresync,
                                    public ::testing::WithParamInterface<Slain>
            {
            };

            TEST_P(SysPresyncSlain, a_thread_slain_in_its_window_leaves_no_record_and_no_stage)
            {
                RunCaps const run = mint(PAGES);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                size_t const before = host_pool_used();
                uint32_t const live0 = kernel().presync_live;
                g_victim = spawn_member();
                ASSERT_NE(g_victim, nullptr);
                RunCaps const mine = share(g_victim, run);
                ASSERT_NE(mine.obj, FRAME_RUN_NONE);
                sys_seam_reset();
                on_window(slay_hook);
                if (setjmp(g_slain) == 0)
                {
                    if (GetParam() == Slain::SYNC)
                    {
                        (void)call_as(g_victim, KOS_SYS_FRAME_MAP, mine.fcap, mine.acap,
                                      va_word(RUN_VA), KOS_MEM_NOCACHE);
                    }
                    else if (GetParam() == Slain::CLEAR)
                    {
                        (void)call_as(g_victim, KOS_SYS_RAM_ALLOC, PAGES * G);
                    }
                    else
                    {
                        kos_task_t t = KOS_TASK_NONE;
                        (void)create_task(g_victim, 0, 0, 0, &t);
                    }
                    FAIL() << "the call opened no window";
                }
                on_window(nullptr);
                running() = g_victim;
                PresyncRecord const& r = record_of(g_victim);
                EXPECT_TRUE(r.live or r.staged != 0) << "slain with nothing in flight";
                run_exit(KOS_EXIT_CANCELLED);
                running() = g_main;
                EXPECT_FALSE(r.live);
                EXPECT_FALSE(r.active);
                EXPECT_EQ(r.staged, 0u);
                EXPECT_EQ(kernel().presync_live, live0);
                EXPECT_EQ(host_pool_used(), before) << "the slain call's frames came back";
            }

            // Longer than two passes of a self-grant, with a short third.
            constexpr uint32_t LONG_PAGES = 2u * ASPACE_MAP_PASS_PAGES + 3u;

            uint32_t mapped_pages(Thread const* t, uintptr_t blk, uint32_t pages)
            {
                struct arch_aspace* const space = domain_space(thread_domain(t));
                uint32_t n = 0;
                for (uint32_t i = 0; i < pages; i++)
                {
                    if (arch_aspace_frame_at(space, blk + i * G) != 0)
                    {
                        n++;
                    }
                }
                return n;
            }

            VirtualRange const* range_of(Thread const* t, uintptr_t blk)
            {
                return domain_ranges(thread_domain(t))->at_base(blk);
            }

            uintptr_t g_pass_blk = 0;
            uint32_t g_pass_last = 0;
            uint32_t g_pass_most = 0;
            uint32_t g_pass_windows = 0;

            void pass_hook(uint32_t)
            {
                if (running() != g_main)
                {
                    return;
                }
                uint32_t const now = mapped_pages(g_main, g_pass_blk, LONG_PAGES);
                if (now - g_pass_last > g_pass_most)
                {
                    g_pass_most = now - g_pass_last;
                }
                g_pass_last = now;
                g_pass_windows++;
            }

            TEST_F(SysPresync, a_long_self_grant_maps_at_most_one_pass_between_two_windows)
            {
                g_pass_blk = reserve_only(LONG_PAGES * G);
                ASSERT_NE(g_pass_blk, 0u);
                sys_seam_reset();
                g_pass_last = 0;
                g_pass_most = 0;
                g_pass_windows = 0;
                on_window(pass_hook);
                EXPECT_EQ(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, g_pass_blk, LONG_PAGES * G, 0),
                          0u);
                on_window(nullptr);
                EXPECT_EQ(g_pass_windows, 2u) << "a window between each two passes";
                EXPECT_EQ(g_pass_most, ASPACE_MAP_PASS_PAGES);
                EXPECT_EQ(mapped_pages(g_main, g_pass_blk, LONG_PAGES), LONG_PAGES);
                VirtualRange const* const e = range_of(g_main, g_pass_blk);
                ASSERT_NE(e, nullptr);
                EXPECT_EQ(e->state, VirtualState::Granted);
                EXPECT_FALSE(vr_pending(e));
            }

            class SysPendingGrant : public SysPresync,
                                    public ::testing::WithParamInterface<Edit>
            {
            };

            TEST_P(SysPendingGrant, keeps_its_frames_from_another_type)
            {
                uintptr_t const blk = reserve_only(LONG_PAGES * G);
                ASSERT_NE(blk, 0u);
                arm(GetParam(), blk, LONG_PAGES * G);
                EXPECT_EQ(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, blk, LONG_PAGES * G, 0), 0u);
                EXPECT_TRUE(g_seen.ran);
                EXPECT_EQ(g_seen.rc, -KOS_EBUSY) << "a non-cacheable mapping of a pending grant";
                EXPECT_EQ(range_of(g_main, blk)->memtype, static_cast<uint8_t>(ARCH_MAP_NORMAL));
            }

            INSTANTIATE_TEST_SUITE_P(Mappings, SysPendingGrant,
                                     ::testing::Values(Edit::SELF_GRANT, Edit::HANDOFF));

            TEST_F(SysPresync, a_pending_self_grant_is_finished_by_a_grant_of_its_type)
            {
                uintptr_t const blk = reserve_only(LONG_PAGES * G);
                ASSERT_NE(blk, 0u);
                arm(Edit::SELF_GRANT_CACHED, blk, LONG_PAGES * G);
                EXPECT_EQ(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, blk, LONG_PAGES * G, 0), 0u);
                EXPECT_TRUE(g_seen.ran);
                EXPECT_EQ(g_seen.rc, 0);
                EXPECT_EQ(mapped_pages(g_main, blk, LONG_PAGES), LONG_PAGES);
                EXPECT_EQ(range_of(g_main, blk)->state, VirtualState::Granted);
            }

            TEST_F(SysPresync, a_granter_slain_mid_grant_leaves_it_pending_and_whole)
            {
                uintptr_t const blk = reserve_only(LONG_PAGES * G);
                ASSERT_NE(blk, 0u);
                g_victim = spawn_member();
                ASSERT_NE(g_victim, nullptr);
                sys_seam_reset();
                on_window(slay_hook);
                if (setjmp(g_slain) == 0)
                {
                    (void)call_as(g_victim, KOS_SYS_MEM_SELF_GRANT, blk, LONG_PAGES * G, 0);
                    FAIL() << "the grant opened no window";
                }
                on_window(nullptr);
                running() = g_victim;
                run_exit(KOS_EXIT_CANCELLED);
                running() = g_main;
                VirtualRange const* const e = range_of(g_main, blk);
                ASSERT_NE(e, nullptr);
                EXPECT_EQ(e->state, VirtualState::Reserved) << "a pending grant is no grant";
                EXPECT_TRUE(vr_pending(e));
                EXPECT_EQ(e->pending_pages, ASPACE_MAP_PASS_PAGES);
                EXPECT_EQ(mapped_pages(g_main, blk, LONG_PAGES), ASPACE_MAP_PASS_PAGES);
                EXPECT_EQ(err(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, blk, LONG_PAGES * G,
                                      KOS_MEM_NOCACHE)),
                          -KOS_EBUSY);
                EXPECT_EQ(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, blk, LONG_PAGES * G, 0), 0u);
                EXPECT_EQ(mapped_pages(g_main, blk, LONG_PAGES), LONG_PAGES);
                EXPECT_EQ(e->state, VirtualState::Granted);
            }

            TEST_F(SysPresync, a_teardown_frees_each_frame_of_a_pending_grant_once)
            {
                uintptr_t const blk = reserve_only(LONG_PAGES * G);
                ASSERT_NE(blk, 0u);
                g_victim = spawn_member();
                ASSERT_NE(g_victim, nullptr);
                sys_seam_reset();
                on_window(slay_hook);
                if (setjmp(g_slain) == 0)
                {
                    (void)call_as(g_victim, KOS_SYS_MEM_SELF_GRANT, blk, LONG_PAGES * G, 0);
                    FAIL() << "the grant opened no window";
                }
                on_window(nullptr);
                running() = g_victim;
                run_exit(KOS_EXIT_CANCELLED);
                running() = g_main;
                size_t const doubles = host_pool_double_frees();
                Domain* const d = thread_domain(g_main);
                {
                    IrqLock lock;
                    aspace_release(d->space, &d->ranges, false);
                    d->space = nullptr;
                }
                EXPECT_EQ(host_pool_double_frees(), doubles);
                for (uint32_t i = 0; i < LONG_PAGES; i++)
                {
                    EXPECT_FALSE(frame_taken(aspace_frame_of(blk) + i * G)) << "page " << i;
                }
            }

            // The pages `d`'s space maps over LONG_PAGES granules from `va`; none once it has no
            // space.
            uint32_t space_mapped(Domain const* d, uintptr_t va)
            {
                struct arch_aspace* const space = domain_space(d);
                uint32_t n = 0;
                for (uint32_t i = 0; space != nullptr and i < LONG_PAGES; i++)
                {
                    if (arch_aspace_frame_at(space, va + i * G) != 0)
                    {
                        n++;
                    }
                }
                return n;
            }

            // The span one thread's call maps or unmaps, and the most pages it moved between two
            // of that thread's windows.
            struct Span
            {
                Thread const* t = nullptr;
                Domain const* d = nullptr;
                uintptr_t va = 0;
                uint32_t last = 0;
                uint32_t most = 0;
                uint32_t windows = 0;
            };
            Span g_span;

            void span_hook(uint32_t)
            {
                if (running() != g_span.t)
                {
                    return;
                }
                uint32_t const now = space_mapped(g_span.d, g_span.va);
                uint32_t moved = now - g_span.last;
                if (now < g_span.last)
                {
                    moved = g_span.last - now;
                }
                if (moved > g_span.most)
                {
                    g_span.most = moved;
                }
                g_span.last = now;
                g_span.windows++;
            }

            void watch_span(Thread const* t, Domain const* d, uintptr_t va)
            {
                sys_seam_reset();
                g_span = Span{t, d, va, space_mapped(d, va), 0, 0};
                on_window(span_hook);
            }

            TEST_F(SysPresync, a_long_frame_map_maps_at_most_one_pass_between_two_windows)
            {
                RunCaps const run = mint(LONG_PAGES);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                Domain const* const d = thread_domain(g_main);
                watch_span(g_main, d, RUN_VA);
                EXPECT_EQ(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, run.acap, va_word(RUN_VA), 0),
                          0u);
                on_window(nullptr);
                EXPECT_EQ(g_span.windows, 2u) << "a window between each two passes";
                EXPECT_EQ(g_span.most, ASPACE_MAP_PASS_PAGES);
                EXPECT_EQ(space_mapped(d, RUN_VA), LONG_PAGES);
                VirtualRange const* const e = domain_ranges(d)->at_base(RUN_VA);
                ASSERT_NE(e, nullptr);
                EXPECT_EQ(e->state, VirtualState::Granted);
                EXPECT_FALSE(vr_pending(e));
            }

            TEST_F(SysPresync, a_long_frame_unmap_unmaps_at_most_one_pass_between_two_windows)
            {
                RunCaps const run = mint(LONG_PAGES);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                uint8_t const refs = run_refs(run.obj);
                ASSERT_EQ(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, run.acap, va_word(RUN_VA), 0),
                          0u);
                Domain const* const d = thread_domain(g_main);
                watch_span(g_main, d, RUN_VA);
                EXPECT_EQ(call_as(g_main, KOS_SYS_FRAME_UNMAP, run.fcap, run.acap, RUN_VA), 0u);
                on_window(nullptr);
                EXPECT_EQ(g_span.windows, 2u) << "a window between each two passes";
                EXPECT_EQ(g_span.most, ASPACE_MAP_PASS_PAGES);
                EXPECT_EQ(space_mapped(d, RUN_VA), 0u);
                EXPECT_EQ(domain_ranges(d)->at_base(RUN_VA), nullptr);
                EXPECT_EQ(run_refs(run.obj), refs) << "the unmap kept the mapping's reference";
            }

            uint32_t g_slay_at = 1;
            uint32_t g_victim_windows = 0;

            // slay_hook at the victim's g_slay_at-th window.
            void slay_nth_hook(uint32_t at)
            {
                if (running() == g_victim and ++g_victim_windows == g_slay_at)
                {
                    slay_hook(at);
                }
            }

            // A member of main's task slain at its frame map's `at`-th window, the run's
            // capability and `into`'s space (main's where null) delegated to it; its exit run
            // where `die`.
            Thread* slay_mid_frame_map(SysPresync* fx, RunCaps const& run, Domain* into = nullptr,
                                       uint32_t at = 1, bool die = true)
            {
                RunCaps own = fx->share_to(g_victim, run);
                if (into != nullptr)
                {
                    own.acap = SysPresync::space_cap_to(g_victim, into);
                }
                if (own.obj == FRAME_RUN_NONE or own.acap == KCAP_INVALID)
                {
                    return nullptr;
                }
                sys_seam_reset();
                g_slay_at = at;
                g_victim_windows = 0;
                on_window(slay_nth_hook);
                if (setjmp(g_slain) == 0)
                {
                    (void)call_as(g_victim, KOS_SYS_FRAME_MAP, own.fcap, own.acap,
                                  va_word(RUN_VA, EDIT_OUT), 0);
                    return nullptr;
                }
                on_window(nullptr);
                running() = g_main;
                if (die)
                {
                    running() = g_victim;
                    run_exit(KOS_EXIT_CANCELLED);
                    running() = g_main;
                }
                return g_victim;
            }

            TEST_F(SysPresync, a_mapper_at_its_window_leaves_its_map_pending_for_a_map_of_its_type)
            {
                RunCaps const run = mint(LONG_PAGES);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                uint8_t const refs = run_refs(run.obj);
                g_victim = spawn_member();
                ASSERT_NE(g_victim, nullptr);
                ASSERT_NE(slay_mid_frame_map(this, run, nullptr, 1, false), nullptr)
                    << "the map opened no window";
                Domain const* const d = thread_domain(g_main);
                VirtualRange const* const e = domain_ranges(d)->at_base(RUN_VA);
                ASSERT_NE(e, nullptr);
                EXPECT_EQ(e->state, VirtualState::Reserved) << "a pending map is no mapping";
                EXPECT_TRUE(vr_pending(e));
                EXPECT_EQ(space_mapped(d, RUN_VA), ASPACE_MAP_PASS_PAGES);
                EXPECT_EQ(err(call_as(g_main, KOS_SYS_FRAME_UNMAP, run.fcap, run.acap, RUN_VA)),
                          -KOS_EBUSY);
                EXPECT_EQ(err(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, run.acap,
                                      va_word(RUN_VA), KOS_MEM_NOCACHE)),
                          -KOS_EBUSY);
                EXPECT_EQ(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, run.acap, va_word(RUN_VA), 0),
                          0u);
                EXPECT_EQ(space_mapped(d, RUN_VA), LONG_PAGES);
                EXPECT_EQ(e->state, VirtualState::Granted);
                // The victim's own capability is the third holder.
                EXPECT_EQ(run_refs(run.obj), refs + 2u) << "a resumed map took a second reference";
                running() = g_victim;
                run_exit(KOS_EXIT_CANCELLED);
                running() = g_main;
                EXPECT_EQ(space_mapped(d, RUN_VA), LONG_PAGES) << "the death took back a mapping "
                                                                  "another map completed";
                EXPECT_EQ(call_as(g_main, KOS_SYS_FRAME_UNMAP, run.fcap, run.acap, RUN_VA), 0u);
                EXPECT_EQ(run_refs(run.obj), refs);
            }

            TEST_F(SysPresync, a_mapper_slain_mid_map_into_another_space_leaves_nothing)
            {
                g_victim = spawn_member();
                ASSERT_NE(g_victim, nullptr);
                running() = g_main;
                kos_task_t const h = new_task(g_main, 0, 0, 0);
                ASSERT_NE(h, KOS_TASK_NONE);
                Thread* const entry = entry_of(h);
                ASSERT_NE(entry, nullptr);
                Domain* const d = thread_domain(entry);
                RunCaps const run = mint(LONG_PAGES);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                uint8_t const refs = run_refs(run.obj);
                running() = g_main;
                ASSERT_NE(slay_mid_frame_map(this, run, d, 2, false), nullptr)
                    << "the map opened no window";
                EXPECT_TRUE(vr_pending(domain_ranges(d)->at_base(RUN_VA)));
                EXPECT_EQ(space_mapped(d, RUN_VA), 2u * ASPACE_MAP_PASS_PAGES);
                watch_span(g_victim, d, RUN_VA);
                running() = g_victim;
                run_exit(KOS_EXIT_CANCELLED);
                running() = g_main;
                on_window(nullptr);
                EXPECT_EQ(g_span.windows, 2u) << "a window after each pass";
                EXPECT_EQ(g_span.most, ASPACE_MAP_PASS_PAGES);
                EXPECT_EQ(space_mapped(d, RUN_VA), 0u);
                EXPECT_EQ(domain_ranges(d)->at_base(RUN_VA), nullptr) << "the map stayed pending";
                EXPECT_EQ(run_refs(run.obj), refs) << "the map kept its reference";
            }

            TEST_F(SysPresync, a_mapper_slain_taking_back_a_map_it_could_not_deliver_leaves_nothing)
            {
                g_victim = spawn_member();
                ASSERT_NE(g_victim, nullptr);
                running() = g_main;
                kos_task_t const h = new_task(g_main, 0, 0, 0);
                ASSERT_NE(h, KOS_TASK_NONE);
                Thread* const entry = entry_of(h);
                ASSERT_NE(entry, nullptr);
                Domain* const d = thread_domain(entry);
                RunCaps const run = mint(LONG_PAGES);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                uint8_t const refs = run_refs(run.obj);
                RunCaps own = share_to(g_victim, run);
                own.acap = space_cap_to(g_victim, d);
                ASSERT_NE(own.obj, FRAME_RUN_NONE);
                ASSERT_NE(own.acap, KCAP_INVALID);
                running() = g_main;
                sys_seam_reset();
                uintptr_t const word = va_word(RUN_VA, EDIT_OUT);
                refuse_acquire(word, 1, 1);
                g_slay_at = 3;
                g_victim_windows = 0;
                on_window(slay_nth_hook);
                if (setjmp(g_slain) == 0)
                {
                    int32_t const rc =
                        err(call_as(g_victim, KOS_SYS_FRAME_MAP, own.fcap, own.acap, word, 0));
                    FAIL() << "the map took nothing back in passes: " << rc;
                }
                on_window(nullptr);
                running() = g_main;
                EXPECT_NE(space_mapped(d, RUN_VA), 0u) << "fixture: nothing left to take back";
                EXPECT_LT(space_mapped(d, RUN_VA), LONG_PAGES) << "fixture: slain before it began";
                running() = g_victim;
                run_exit(KOS_EXIT_CANCELLED);
                running() = g_main;
                EXPECT_EQ(space_mapped(d, RUN_VA), 0u) << "the undelivered map outlived its mapper";
                EXPECT_EQ(domain_ranges(d)->at_base(RUN_VA), nullptr);
                EXPECT_EQ(run_refs(run.obj), refs) << "the map kept its reference";
            }

            uint32_t g_refuse_at = 0;
            uint32_t g_close_cap = KCAP_INVALID;

            // span_hook, the map refused, or its space capability closed, at the g_refuse_at-th
            // window.
            void refuse_span_hook(uint32_t at)
            {
                span_hook(at);
                if (running() != g_span.t or g_span.windows != g_refuse_at)
                {
                    return;
                }
                if (g_close_cap != KCAP_INVALID)
                {
                    (void)SysPresync::close_cap(g_main, g_close_cap);
                    return;
                }
                refuse_map(true);
            }

            class SysMapLost : public SysPresync, public ::testing::WithParamInterface<bool>
            {
            };

            TEST_P(SysMapLost, a_frame_map_refused_or_cut_off_past_its_first_pass_leaves_nothing)
            {
                bool const closed = GetParam();
                kos_task_t const h = new_task(g_main, 0, 0, 0);
                ASSERT_NE(h, KOS_TASK_NONE);
                Thread* const entry = entry_of(h);
                ASSERT_NE(entry, nullptr);
                Domain* const d = thread_domain(entry);
                RunCaps const run = mint(LONG_PAGES);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                uint8_t const refs = run_refs(run.obj);
                uint32_t const acap = space_cap(g_main, d);
                ASSERT_NE(acap, KCAP_INVALID);
                running() = g_main;
                watch_span(g_main, d, RUN_VA);
                g_refuse_at = 2;
                g_close_cap = KCAP_INVALID;
                if (closed)
                {
                    g_close_cap = acap;
                }
                on_window(refuse_span_hook);
                int32_t const rc = err(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, acap,
                                               va_word(RUN_VA), 0));
                on_window(nullptr);
                refuse_map(false);
                if (closed)
                {
                    EXPECT_EQ(rc, -KOS_EBADF);
                }
                else
                {
                    EXPECT_EQ(rc, -KOS_ENOMEM);
                }
                EXPECT_EQ(g_span.windows, 4u) << "a window after each pass, the unwind's too";
                EXPECT_EQ(g_span.most, ASPACE_MAP_PASS_PAGES);
                EXPECT_EQ(space_mapped(d, RUN_VA), 0u);
                EXPECT_EQ(domain_ranges(d)->at_base(RUN_VA), nullptr) << "the map stayed pending";
                EXPECT_EQ(run_refs(run.obj), refs) << "the map kept its reference";
            }

            INSTANTIATE_TEST_SUITE_P(Cut, SysMapLost, ::testing::Values(false, true));

            // The map refused at the first window that finds pages of it mapped.
            void refuse_once_mapped_hook(uint32_t)
            {
                if (running() != g_span.t)
                {
                    return;
                }
                uint32_t const now = space_mapped(g_span.d, g_span.va);
                if (now > g_span.most)
                {
                    g_span.most = now;
                }
                if (now != 0)
                {
                    refuse_map(true);
                }
            }

            TEST_F(SysPresync, an_uncached_frame_map_refused_mid_passes_leaves_the_next_cacheable_a_sync)
            {
                RunCaps const run = mint(LONG_PAGES);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                running() = g_main;
                Domain* const d = thread_domain(g_main);
                sys_seam_reset();
                g_span = Span{g_main, d, RUN_VA, 0, 0, 0};
                on_window(refuse_once_mapped_hook);
                int32_t const rc = err(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, run.acap,
                                               va_word(RUN_VA), KOS_MEM_NOCACHE));
                on_window(nullptr);
                refuse_map(false);
                EXPECT_EQ(rc, -KOS_ENOMEM);
                EXPECT_GE(g_span.most, ASPACE_MAP_PASS_PAGES) << "fixture: refused before a pass";
                EXPECT_EQ(space_mapped(d, RUN_VA), 0u);
                EXPECT_TRUE(frame_run_sync_owed(run.obj)) << "uncached pages went unrecorded";
                sys_seam_reset();
                EXPECT_EQ(err(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, run.acap,
                                      va_word(RUN_VA), 0)),
                          0);
                EXPECT_EQ(g_syncs, LONG_PAGES) << "the cacheable map after it";
            }

            RunCaps g_finisher_run;
            Thread* g_finisher = nullptr;
            int32_t g_finisher_rc = 1;

            // Another holder of the run maps it at the same address and type, finishing the map.
            void finish_hook(uint32_t)
            {
                if (running() != g_main or g_finisher_rc != 1)
                {
                    return;
                }
                g_finisher_rc = err(call_as(g_finisher, KOS_SYS_FRAME_MAP, g_finisher_run.fcap,
                                            g_finisher_run.acap, va_word(RUN_VA, EDIT_OUT), 0));
                running() = g_main;
            }

            TEST_F(SysPresync, a_teardown_unmaps_a_pending_frame_map_and_gives_its_reference_back)
            {
                RunCaps const run = mint(LONG_PAGES);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                uint8_t const refs = run_refs(run.obj);
                g_victim = spawn_member();
                ASSERT_NE(g_victim, nullptr);
                ASSERT_NE(slay_mid_frame_map(this, run, nullptr, 1, false), nullptr)
                    << "the map opened no window";
                size_t const doubles = host_pool_double_frees();
                Domain* const d = thread_domain(g_main);
                {
                    IrqLock lock;
                    aspace_release(d->space, &d->ranges, false);
                    d->space = nullptr;
                }
                running() = g_victim;
                run_exit(KOS_EXIT_CANCELLED);
                running() = g_main;
                EXPECT_EQ(host_pool_double_frees(), doubles);
                EXPECT_EQ(run_refs(run.obj), refs);
                for (uint32_t i = 0; i < LONG_PAGES; i++)
                {
                    EXPECT_TRUE(frame_taken(run.base + i * G)) << "the run's page " << i;
                }
            }

            TEST_F(SysPresync, an_unmapper_slain_mid_unmap_leaves_a_shorter_mapping_to_unmap)
            {
                RunCaps const run = mint(LONG_PAGES);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                uint8_t const refs = run_refs(run.obj);
                ASSERT_EQ(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, run.acap, va_word(RUN_VA), 0),
                          0u);
                g_victim = spawn_member();
                ASSERT_NE(g_victim, nullptr);
                RunCaps const own = share_to(g_victim, run);
                ASSERT_NE(own.obj, FRAME_RUN_NONE);
                sys_seam_reset();
                on_window(slay_hook);
                if (setjmp(g_slain) == 0)
                {
                    (void)call_as(g_victim, KOS_SYS_FRAME_UNMAP, own.fcap, own.acap, RUN_VA);
                    FAIL() << "the unmap opened no window";
                }
                on_window(nullptr);
                running() = g_victim;
                run_exit(KOS_EXIT_CANCELLED);
                running() = g_main;
                Domain const* const d = thread_domain(g_main);
                VirtualRange const* const e = domain_ranges(d)->at_base(RUN_VA);
                ASSERT_NE(e, nullptr);
                EXPECT_EQ(e->state, VirtualState::Granted);
                EXPECT_EQ(e->pages, LONG_PAGES - ASPACE_MAP_PASS_PAGES);
                EXPECT_EQ(space_mapped(d, RUN_VA), LONG_PAGES - ASPACE_MAP_PASS_PAGES);
                EXPECT_EQ(call_as(g_main, KOS_SYS_FRAME_UNMAP, run.fcap, run.acap, RUN_VA), 0u);
                EXPECT_EQ(space_mapped(d, RUN_VA), 0u);
                EXPECT_EQ(run_refs(run.obj), refs);
            }

            TEST_F(SysPresync, an_exit_unmaps_a_long_window_at_most_one_pass_between_two_windows)
            {
                uintptr_t const blk = reserve_only(LONG_PAGES * G);
                ASSERT_NE(blk, 0u);
                kos_thread_t h = KOS_THREAD_NONE;
                ASSERT_EQ(spawn(g_main, spawn_params(EDIT_PARAMS, blk, LONG_PAGES * G, 0), EDIT_OUT,
                                &h),
                          0);
                Thread* const child = kernel().threads.resolve(h);
                ASSERT_NE(child, nullptr);
                Domain* const d = thread_domain(g_main);
                uint16_t const holder = static_cast<uint16_t>(kernel().threads.index_of(child) + 1);
                uintptr_t wva = 0;
                for (size_t i = 0; i < VirtualRanges::capacity(); i++)
                {
                    VirtualRange const* const e = d->ranges.at(i);
                    if (e != nullptr and (e->flags & VR_WINDOW) != 0 and e->holder == holder)
                    {
                        wva = e->base;
                    }
                }
                ASSERT_NE(wva, 0u) << "fixture: the spawn mapped no window";
                ASSERT_EQ(space_mapped(d, wva), LONG_PAGES);
                watch_span(child, d, wva);
                exit_as(child);
                on_window(nullptr);
                EXPECT_EQ(g_span.windows, 3u) << "a window after each pass";
                EXPECT_EQ(g_span.most, ASPACE_MAP_PASS_PAGES);
                EXPECT_EQ(space_mapped(d, wva), 0u);
                EXPECT_EQ(d->ranges.at_base(wva), nullptr);
                EXPECT_EQ(d->ranges.at_base(blk)->lends, 0u) << "the window kept its lend";
            }

            // What the last member's exit leaves its space to trim: a reservation of its own, its
            // creator's block handed over, and a run its creator mapped.
            enum class Trim
            {
                OWNED,
                HANDOFF,
                RUN
            };

            class SysTrim : public SysPresync, public ::testing::WithParamInterface<Trim>
            {
            };

            TEST_P(SysTrim, the_last_member_s_exit_trims_at_most_one_pass_between_two_windows)
            {
                uintptr_t blk = 0;
                if (GetParam() == Trim::HANDOFF)
                {
                    blk = reserve_only(LONG_PAGES * G);
                    ASSERT_NE(blk, 0u);
                }
                kos_task_t const h = new_task(g_main, blk, LONG_PAGES * G, 0);
                ASSERT_NE(h, KOS_TASK_NONE);
                Thread* const entry = entry_of(h);
                ASSERT_NE(entry, nullptr);
                Domain* const d = thread_domain(entry);
                RunCaps run;
                uint8_t refs = 0;
                if (GetParam() == Trim::OWNED)
                {
                    {
                        IrqLock lock;
                        cap_seat_authority(entry, AUTH_MEMORY);
                    }
                    blk = call_as(entry, KOS_SYS_RAM_ALLOC, LONG_PAGES * G);
                    ASSERT_NE(blk, 0u);
                    ASSERT_EQ(call_as(entry, KOS_SYS_MEM_SELF_GRANT, blk, LONG_PAGES * G, 0), 0u);
                }
                if (GetParam() == Trim::RUN)
                {
                    run = mint(LONG_PAGES);
                    ASSERT_NE(run.obj, FRAME_RUN_NONE);
                    refs = run_refs(run.obj);
                    uint32_t const acap = space_cap(g_main, d);
                    ASSERT_EQ(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, acap, va_word(RUN_VA),
                                      0),
                              0u);
                    blk = RUN_VA;
                }
                running() = g_main;
                ASSERT_EQ(space_mapped(d, blk), LONG_PAGES);
                watch_span(entry, d, blk);
                exit_as(entry);
                on_window(nullptr);
                EXPECT_EQ(g_span.windows, 3u) << "a window after each pass";
                EXPECT_EQ(g_span.most, ASPACE_MAP_PASS_PAGES);
                ASSERT_NE(domain_space(d), nullptr) << "fixture: the creator hold kept no space";
                EXPECT_EQ(space_mapped(d, blk), 0u);
                for (uint32_t i = 0; GetParam() == Trim::OWNED and i < LONG_PAGES; i++)
                {
                    EXPECT_FALSE(frame_taken(aspace_frame_of(blk) + i * G)) << "page " << i;
                }
                if (GetParam() == Trim::RUN)
                {
                    EXPECT_EQ(run_refs(run.obj), refs) << "the mapping kept its reference";
                }
                size_t const doubles = host_pool_double_frees();
                ASSERT_EQ(call_as(g_main, KOS_SYS_TASK_SLAY, h), 0u);
                EXPECT_EQ(domain_space(d), nullptr);
                EXPECT_EQ(host_pool_double_frees(), doubles);
                for (uint32_t i = 0; GetParam() == Trim::HANDOFF and i < LONG_PAGES; i++)
                {
                    EXPECT_TRUE(frame_taken(aspace_frame_of(blk) + i * G)) << "page " << i;
                }
            }

            INSTANTIATE_TEST_SUITE_P(Spans, SysTrim,
                                     ::testing::Values(Trim::OWNED, Trim::HANDOFF, Trim::RUN));

            // The thread a create built, not yet runnable, whose window or handoff over `blk`
            // its pending maps reach, and the domain they are in; null where none is.
            Thread* building_over(uintptr_t blk, Domain** in)
            {
                for (Domain& d : kernel().domains())
                {
                    if (domain_space(&d) == nullptr)
                    {
                        continue;
                    }
                    for (size_t i = 0; i < VirtualRanges::capacity(); i++)
                    {
                        VirtualRange const* const e = d.ranges.at(i);
                        if (e == nullptr or (e->flags & VR_BORROWED) == 0
                            or (e->flags & VR_FRAMECAP) != 0)
                        {
                            continue;
                        }
                        uintptr_t const at = aspace_user_va(arch_aspace_frame_at(d.space, e->base));
                        if (at != blk)
                        {
                            continue;
                        }
                        *in = &d;
                        if ((e->flags & VR_WINDOW) != 0)
                        {
                            return &kernel().threads.slots()[e->holder - 1u];
                        }
                        for (Thread& th : kernel().threads.all())
                        {
                            if (thread_domain(&th) == &d and th.task_role == TASK_ROLE_ENTRY)
                            {
                                return &th;
                            }
                        }
                    }
                }
                return nullptr;
            }

            // The pages a create's maps hold of `blk`'s frames, wherever they are mapped.
            uint32_t create_mapped(uintptr_t blk)
            {
                Domain* d = nullptr;
                if (building_over(blk, &d) == nullptr)
                {
                    return 0;
                }
                for (size_t i = 0; i < VirtualRanges::capacity(); i++)
                {
                    VirtualRange const* const e = d->ranges.at(i);
                    if (e != nullptr and (e->flags & VR_BORROWED) != 0
                        and aspace_user_va(arch_aspace_frame_at(d->space, e->base)) == blk)
                    {
                        return vr_mapped(e);
                    }
                }
                return 0;
            }

            // What a create's pass windows saw: the most pages mapped between two, and whether
            // at each the child was not runnable and its creator's word still named nothing.
            struct CreateSeen
            {
                uintptr_t blk = 0;
                uintptr_t word = 0;
                uint32_t last = 0;
                uint32_t most = 0;
                uint32_t windows = 0;
                bool hidden = true;
                bool refuse_at_first = false;
            };
            CreateSeen g_create;

            void create_hook(uint32_t)
            {
                Domain* d = nullptr;
                Thread* const child = building_over(g_create.blk, &d);
                if (child == nullptr or running() == child)
                {
                    return;
                }
                uint32_t const now = create_mapped(g_create.blk);
                if (now - g_create.last > g_create.most)
                {
                    g_create.most = now - g_create.last;
                }
                g_create.last = now;
                g_create.windows++;
                uint32_t const named = *host<uint32_t>(g_create.word);
                if (child->state != ThreadState::INACTIVE
                    or (named != KOS_THREAD_NONE and named != KOS_TASK_NONE))
                {
                    g_create.hidden = false;
                }
                if (g_create.refuse_at_first)
                {
                    refuse_map(true);
                    g_create.refuse_at_first = false;
                }
            }

            void watch_create(uintptr_t blk, uintptr_t word, bool refuse)
            {
                sys_seam_reset();
                g_create = CreateSeen{};
                g_create.blk = blk;
                g_create.word = word;
                g_create.last = ASPACE_MAP_PASS_PAGES;
                g_create.refuse_at_first = refuse;
                on_window(create_hook);
            }

            // What a create left of `blk`'s frames once the thread it built ran its death: no
            // mapping, no lend, each frame freed at most once.
            void expect_create_gone(Thread* child, uintptr_t blk, size_t doubles)
            {
                running() = child;
                run_exit(KOS_EXIT_CANCELLED);
                running() = g_main;
                Domain* d = nullptr;
                EXPECT_EQ(building_over(blk, &d), nullptr) << "a mapping of the create stayed";
                EXPECT_EQ(domain_ranges(thread_domain(g_main))->at_base(blk)->lends, 0u);
                EXPECT_EQ(host_pool_double_frees(), doubles);
            }

            TEST_F(SysPresync, a_long_window_spawn_names_and_starts_its_child_after_its_passes)
            {
                uintptr_t const blk = reserve_only(LONG_PAGES * G);
                ASSERT_NE(blk, 0u);
                watch_create(blk, uva(MAIN_OUT), false);
                kos_thread_t h = KOS_THREAD_NONE;
                EXPECT_EQ(spawn(g_main, spawn_params(EDIT_PARAMS, blk, LONG_PAGES * G, 0), MAIN_OUT,
                                &h),
                          0);
                on_window(nullptr);
                EXPECT_EQ(g_create.windows, 2u) << "a window after each pass";
                EXPECT_EQ(g_create.most, ASPACE_MAP_PASS_PAGES);
                EXPECT_TRUE(g_create.hidden) << "the child ran or was named before its maps";
                Thread* const child = kernel().threads.resolve(h);
                ASSERT_NE(child, nullptr);
                EXPECT_NE(child->state, ThreadState::INACTIVE);
                EXPECT_EQ(create_mapped(blk), LONG_PAGES);
                EXPECT_EQ(record_of(g_main).building, 0u);
            }

            TEST_F(SysPresync, a_long_handoff_create_names_and_starts_its_task_after_its_passes)
            {
                uintptr_t const blk = reserve_only(LONG_PAGES * G);
                ASSERT_NE(blk, 0u);
                watch_create(blk, uva(MAIN_OUT), false);
                kos_task_t t = KOS_TASK_NONE;
                EXPECT_EQ(create_task(g_main, blk, LONG_PAGES * G, 0, &t), 0);
                on_window(nullptr);
                EXPECT_EQ(g_create.windows, 2u) << "a window after each pass";
                EXPECT_EQ(g_create.most, ASPACE_MAP_PASS_PAGES);
                EXPECT_TRUE(g_create.hidden) << "the entry ran or the task was named before its maps";
                ASSERT_NE(t, KOS_TASK_NONE);
                Thread* const entry = entry_of(t);
                ASSERT_NE(entry, nullptr);
                EXPECT_NE(entry->state, ThreadState::INACTIVE);
                EXPECT_EQ(create_mapped(blk), LONG_PAGES);
            }

            TEST_F(SysPresync, a_window_spawn_refused_mid_passes_fails_unnamed_and_leaves_nothing)
            {
                uintptr_t const blk = reserve_only(LONG_PAGES * G);
                ASSERT_NE(blk, 0u);
                watch_create(blk, uva(MAIN_OUT), true);
                kos_thread_t h = KOS_THREAD_NONE;
                EXPECT_EQ(spawn(g_main, spawn_params(EDIT_PARAMS, blk, LONG_PAGES * G, 0), MAIN_OUT,
                                &h),
                          -KOS_ENOMEM);
                refuse_map(false);
                on_window(nullptr);
                EXPECT_EQ(h, KOS_THREAD_NONE) << "a refused create named its child";
                Domain* d = nullptr;
                Thread* const child = building_over(blk, &d);
                ASSERT_NE(child, nullptr);
                EXPECT_EQ(child->cancel_kind, CANCEL_SLAY) << "the child was left runnable";
                EXPECT_EQ(child->task_role, TASK_ROLE_MEMBER);
                EXPECT_EQ(record_of(g_main).building, 0u);
                expect_create_gone(child, blk, host_pool_double_frees());
            }

            TEST_F(SysPresync, a_handoff_create_refused_mid_passes_fails_unnamed_and_leaves_nothing)
            {
                uintptr_t const blk = reserve_only(LONG_PAGES * G);
                ASSERT_NE(blk, 0u);
                int const tasks = created_tasks();
                watch_create(blk, uva(MAIN_OUT), true);
                kos_task_t t = KOS_TASK_NONE;
                EXPECT_EQ(create_task(g_main, blk, LONG_PAGES * G, 0, &t), -KOS_ENOMEM);
                refuse_map(false);
                on_window(nullptr);
                EXPECT_EQ(t, KOS_TASK_NONE) << "a refused create named its task";
                EXPECT_EQ(created_tasks(), tasks) << "the refused create kept its creator hold";
                Domain* d = nullptr;
                Thread* const entry = building_over(blk, &d);
                ASSERT_NE(entry, nullptr);
                EXPECT_EQ(entry->cancel_kind, CANCEL_SLAY) << "the entry was left runnable";
                expect_create_gone(entry, blk, host_pool_double_frees());
            }

            uintptr_t g_slay_blk = 0;

            // slay_hook, once the create has built what its passes map over g_slay_blk.
            void slay_built_hook(uint32_t at)
            {
                Domain* d = nullptr;
                if (building_over(g_slay_blk, &d) != nullptr)
                {
                    slay_hook(at);
                }
            }

            TEST_F(SysPresync, a_spawner_slain_mid_passes_leaves_its_child_to_die_unrun)
            {
                uintptr_t const blk = reserve_only(LONG_PAGES * G);
                ASSERT_NE(blk, 0u);
                g_victim = spawn_member();
                ASSERT_NE(g_victim, nullptr);
                sys_seam_reset();
                g_slay_blk = blk;
                on_window(slay_built_hook);
                if (setjmp(g_slain) == 0)
                {
                    (void)call_as(g_victim, KOS_SYS_THREAD_CREATE,
                                  spawn_params(EDIT_PARAMS, blk, LONG_PAGES * G, 0),
                                  uva(EDIT_OUT));
                    FAIL() << "the spawn opened no window";
                }
                on_window(nullptr);
                Domain* d = nullptr;
                Thread* const child = building_over(blk, &d);
                ASSERT_NE(child, nullptr);
                EXPECT_EQ(child->state, ThreadState::INACTIVE);
                size_t const doubles = host_pool_double_frees();
                running() = g_victim;
                run_exit(KOS_EXIT_CANCELLED);
                running() = g_main;
                EXPECT_EQ(child->cancel_kind, CANCEL_SLAY) << "the spawner's death left its child";
                EXPECT_NE(child->state, ThreadState::INACTIVE);
                expect_create_gone(child, blk, doubles);
            }

            kos_task_t g_stop_handle = KOS_TASK_NONE;

            // The create's own task slain at its first window past the bracket, its creator
            // then dying there.
            void stop_built_hook(uint32_t at)
            {
                Domain* d = nullptr;
                if (running() != g_victim or building_over(g_slay_blk, &d) == nullptr)
                {
                    return;
                }
                // Slain by main, its creator, through the handle main already holds.
                EXPECT_EQ(call_as(g_main, KOS_SYS_TASK_SLAY, g_stop_handle), 0u);
                running() = g_victim;
                slay_hook(at);
            }

            TEST_F(SysPresync, a_spawn_whose_task_is_slain_mid_passes_leaves_its_child_once)
            {
                // Main's user pages handed to the creator, whose calls read and write them.
                kos_task_t const h = new_task(g_main, user_, USER_PAGES * G, 0);
                ASSERT_NE(h, KOS_TASK_NONE);
                g_victim = entry_of(h);
                ASSERT_NE(g_victim, nullptr);
                {
                    IrqLock lock;
                    cap_seat_authority(g_victim, AUTH_MEMORY | AUTH_TASKS);
                }
                g_stop_handle = h;
                uintptr_t const blk = call_as(g_victim, KOS_SYS_RAM_ALLOC, LONG_PAGES * G);
                ASSERT_NE(blk, 0u);
                running() = g_main;
                sys_seam_reset();
                g_slay_blk = blk;
                on_window(stop_built_hook);
                if (setjmp(g_slain) == 0)
                {
                    (void)call_as(g_victim, KOS_SYS_THREAD_CREATE,
                                  spawn_params(EDIT_PARAMS, blk, LONG_PAGES * G, 0),
                                  uva(EDIT_OUT));
                    FAIL() << "the spawn opened no window";
                }
                on_window(nullptr);
                Domain* d = nullptr;
                Thread* const child = building_over(blk, &d);
                ASSERT_NE(child, nullptr);
                EXPECT_EQ(child->state, ThreadState::READY) << "the slay left the child unadded";
                EXPECT_EQ(child->cancel_kind, CANCEL_SLAY);
                running() = g_victim;
                run_exit(KOS_EXIT_CANCELLED);
                running() = g_main;
                size_t const doubles = host_pool_double_frees();
                running() = child;
                run_exit(KOS_EXIT_CANCELLED);
                running() = g_main;
                EXPECT_EQ(building_over(blk, &d), nullptr) << "a mapping of the create stayed";
                EXPECT_EQ(host_pool_double_frees(), doubles);
            }

            Task* g_gap_task = nullptr;
            uint32_t g_built_windows = 0;

            // Another core's slay of the task a create builds, in the gap past its last pass.
            void slay_at_gap()
            {
                IrqLock lock;
                task_stop_for_slay(g_gap_task, lock);
            }

            void slay_past_last_pass_hook(uint32_t)
            {
                Domain* d = nullptr;
                Thread* const entry = building_over(g_slay_blk, &d);
                if (entry == nullptr)
                {
                    return;
                }
                g_built_windows++;
                if (g_built_windows == 2)
                {
                    g_gap_task = entry->task;
                    run_in_chunk_gap(slay_at_gap, 1);
                }
            }

            TEST_F(SysPresync, a_create_whose_task_is_slain_past_its_last_pass_starts_it_once)
            {
                uintptr_t const blk = reserve_only(LONG_PAGES * G);
                ASSERT_NE(blk, 0u);
                sys_seam_reset();
                g_slay_blk = blk;
                g_gap_task = nullptr;
                g_built_windows = 0;
                on_window(slay_past_last_pass_hook);
                kos_task_t t = KOS_TASK_NONE;
                EXPECT_EQ(create_task(g_main, blk, LONG_PAGES * G, 0, &t), -KOS_ECANCELED);
                on_window(nullptr);
                ASSERT_NE(g_gap_task, nullptr) << "fixture: no gap past the last pass";
                EXPECT_EQ(t, KOS_TASK_NONE) << "a cancelled create named its task";
                Domain* d = nullptr;
                Thread* const entry = building_over(blk, &d);
                ASSERT_NE(entry, nullptr);
                EXPECT_EQ(entry->cancel_kind, CANCEL_SLAY);
                expect_create_gone(entry, blk, host_pool_double_frees());
            }

            // The create's entry, started slain at its first window and dead, its slot then
            // seated again by another create left at its own first window.
            struct Recycled
            {
                uintptr_t blk = 0;
                uintptr_t other_blk = 0;
                uintptr_t plain = 0;
                uintptr_t other_params = 0;
                uintptr_t out = 0;
                Thread* other = nullptr;
                int slot = -1;
                Thread* seated = nullptr;
            };
            Recycled g_recycle;
            constexpr uint32_t SHORT_PAGES = ASPACE_MAP_PASS_PAGES + 3u;

            void other_slain_hook(uint32_t)
            {
                if (running() == g_recycle.other)
                {
                    longjmp(g_slain, 1);
                }
            }

            void recycle_hook(uint32_t)
            {
                Domain* d = nullptr;
                Thread* const entry = building_over(g_recycle.blk, &d);
                if (g_recycle.slot >= 0 or entry == nullptr or running() != g_main)
                {
                    return;
                }
                g_recycle.slot = kernel().threads.index_of(entry);
                {
                    IrqLock lock;
                    task_stop_for_slay(entry->task, lock);
                }
                running() = entry;
                run_exit(KOS_EXIT_CANCELLED);
                // Every other free slot claimed, so the next claim is the entry's. Not by main,
                // whose record holds the create under way.
                uint32_t* const out = host<uint32_t>(g_recycle.out);
                while (true)
                {
                    *out = KOS_THREAD_NONE;
                    running() = g_main;
                    if (err(call_as(g_recycle.other, KOS_SYS_THREAD_CREATE, g_recycle.plain,
                                    g_recycle.out))
                        != 0)
                    {
                        return;
                    }
                    Thread* const filler = kernel().threads.resolve(static_cast<int>(*out));
                    if (kernel().threads.index_of(filler) == g_recycle.slot)
                    {
                        running() = filler;
                        run_exit(0);
                        break;
                    }
                }
                running() = g_main;
                on_window(other_slain_hook);
                if (setjmp(g_slain) == 0)
                {
                    (void)call_as(g_recycle.other, KOS_SYS_THREAD_CREATE, g_recycle.other_params,
                                  g_recycle.out);
                }
                on_window(nullptr);
                running() = g_main;
                Thread* const seated = &kernel().threads.slots()[g_recycle.slot];
                if (seated->state == ThreadState::INACTIVE)
                {
                    g_recycle.seated = seated;
                }
            }

            TEST_F(SysPresync, a_create_whose_entry_slot_is_seated_again_mid_passes_is_cancelled)
            {
                g_recycle = Recycled{};
                g_recycle.blk = reserve_only(SHORT_PAGES * G);
                g_recycle.other_blk = reserve_only(SHORT_PAGES * G);
                ASSERT_NE(g_recycle.blk, 0u);
                ASSERT_NE(g_recycle.other_blk, 0u);
                g_recycle.other = spawn_member();
                ASSERT_NE(g_recycle.other, nullptr);
                g_recycle.plain = spawn_params(MAIN_PARAMS, 0, 0, 0);
                g_recycle.other_params =
                    spawn_params(EDIT_PARAMS, g_recycle.other_blk, SHORT_PAGES * G, 0);
                g_recycle.out = uva(EDIT_OUT);
                running() = g_main;
                sys_seam_reset();
                on_window(recycle_hook);
                kos_task_t t = KOS_TASK_NONE;
                EXPECT_EQ(create_task(g_main, g_recycle.blk, SHORT_PAGES * G, 0, &t),
                          -KOS_ECANCELED);
                on_window(nullptr);
                ASSERT_NE(g_recycle.seated, nullptr) << "fixture: the slot was not seated again";
                EXPECT_EQ(t, KOS_TASK_NONE) << "the old create named another thread's task";
                Thread* const seated = g_recycle.seated;
                EXPECT_EQ(seated->state, ThreadState::INACTIVE) << "the old create started it";
                EXPECT_EQ(seated->cancel_kind, CANCEL_NONE);
                size_t const doubles = host_pool_double_frees();
                running() = g_recycle.other;
                run_exit(KOS_EXIT_CANCELLED);
                running() = g_main;
                EXPECT_EQ(seated->cancel_kind, CANCEL_SLAY);
                expect_create_gone(seated, g_recycle.other_blk, doubles);
            }

            TEST_F(SysPresync, a_creator_slain_mid_passes_leaves_its_task_to_die_unrun)
            {
                uintptr_t const blk = reserve_only(LONG_PAGES * G);
                ASSERT_NE(blk, 0u);
                g_victim = spawn_member();
                ASSERT_NE(g_victim, nullptr);
                int const tasks = created_tasks();
                sys_seam_reset();
                g_slay_blk = blk;
                on_window(slay_built_hook);
                if (setjmp(g_slain) == 0)
                {
                    kos_task_t t = KOS_TASK_NONE;
                    (void)create_task(g_victim, blk, LONG_PAGES * G, 0, &t, {}, EDIT_PARAMS,
                                      EDIT_OUT);
                    FAIL() << "the create opened no window";
                }
                on_window(nullptr);
                Domain* d = nullptr;
                Thread* const entry = building_over(blk, &d);
                ASSERT_NE(entry, nullptr);
                EXPECT_EQ(entry->state, ThreadState::INACTIVE);
                size_t const doubles = host_pool_double_frees();
                running() = g_victim;
                run_exit(KOS_EXIT_CANCELLED);
                running() = g_main;
                EXPECT_EQ(created_tasks(), tasks) << "the creator's death kept its hold";
                EXPECT_EQ(entry->cancel_kind, CANCEL_SLAY) << "the creator's death left its task";
                expect_create_gone(entry, blk, doubles);
            }

            // The frames of `blk` still taken, and what `d`'s space maps of them.
            uint32_t held_pages(Domain const* d, uintptr_t blk)
            {
                uint32_t n = space_mapped(d, blk);
                for (uint32_t i = 0; i < LONG_PAGES; i++)
                {
                    if (host_pool_mark().used[(aspace_frame_of(blk) + i * G - HOST_POOL_LO) / G])
                    {
                        n++;
                    }
                }
                return n;
            }

            Domain const* g_reap_space = nullptr;
            uintptr_t g_reap_blk = 0;
            Thread const* g_reap_thread = nullptr;
            uint32_t g_reap_last = 0;
            uint32_t g_reap_most = 0;
            uint32_t g_reap_windows = 0;

            void reap_hook(uint32_t)
            {
                if (running() != g_reap_thread)
                {
                    return;
                }
                uint32_t const now = held_pages(g_reap_space, g_reap_blk);
                if (g_reap_last - now > g_reap_most)
                {
                    g_reap_most = g_reap_last - now;
                }
                g_reap_last = now;
                g_reap_windows++;
            }

            TEST_F(SysPresync, the_last_borrower_of_a_shed_reservation_frees_it_in_passes)
            {
                // Main's user pages handed to the lender, whose calls read and write them.
                kos_task_t const a = new_task(g_main, user_, USER_PAGES * G, 0);
                ASSERT_NE(a, KOS_TASK_NONE);
                Thread* const lender = entry_of(a);
                ASSERT_NE(lender, nullptr);
                {
                    IrqLock lock;
                    cap_seat_authority(lender, AUTH_MEMORY | AUTH_TASKS);
                }
                uintptr_t const blk = call_as(lender, KOS_SYS_RAM_ALLOC, LONG_PAGES * G);
                ASSERT_NE(blk, 0u);
                kos_task_t b = KOS_TASK_NONE;
                ASSERT_EQ(create_task(lender, blk, LONG_PAGES * G, 0, &b, {}, EDIT_PARAMS,
                                      EDIT_OUT),
                          0);
                Thread* const borrower = entry_of(b);
                ASSERT_NE(borrower, nullptr);
                Domain* const shed = thread_domain(lender);
                running() = g_main;
                exit_as(lender);
                ASSERT_EQ(call_as(g_main, KOS_SYS_TASK_SLAY, a), 0u);
                ASSERT_EQ(domain_space(shed), nullptr) << "fixture: the lender's space is not shed";
                ASSERT_NE(shed->ranges.at_base(blk), nullptr);
                Domain const* const d = thread_domain(borrower);
                sys_seam_reset();
                g_reap_space = d;
                g_reap_blk = blk;
                g_reap_thread = borrower;
                g_reap_last = held_pages(d, blk);
                g_reap_most = 0;
                g_reap_windows = 0;
                ASSERT_EQ(g_reap_last, 2u * LONG_PAGES);
                size_t const doubles = host_pool_double_frees();
                on_window(reap_hook);
                exit_as(borrower);
                on_window(nullptr);
                EXPECT_EQ(g_reap_windows, 6u) << "a window after each pass";
                EXPECT_EQ(g_reap_most, ASPACE_MAP_PASS_PAGES);
                for (uint32_t i = 0; i < LONG_PAGES; i++)
                {
                    EXPECT_FALSE(frame_taken(aspace_frame_of(blk) + i * G)) << "page " << i;
                }
                EXPECT_EQ(host_pool_double_frees(), doubles);
            }

            void watch_reap(Thread const* t, Domain const* d, uintptr_t blk)
            {
                sys_seam_reset();
                g_reap_space = d;
                g_reap_blk = blk;
                g_reap_thread = t;
                g_reap_last = held_pages(d, blk);
                g_reap_most = 0;
                g_reap_windows = 0;
                on_window(reap_hook);
            }

            TEST_F(SysPresync, the_last_member_s_exit_frees_an_unmapped_reservation_in_passes)
            {
                kos_task_t const h = new_task(g_main, 0, 0, 0);
                ASSERT_NE(h, KOS_TASK_NONE);
                Thread* const entry = entry_of(h);
                ASSERT_NE(entry, nullptr);
                {
                    IrqLock lock;
                    cap_seat_authority(entry, AUTH_MEMORY);
                }
                uintptr_t const blk = call_as(entry, KOS_SYS_RAM_ALLOC, LONG_PAGES * G);
                ASSERT_NE(blk, 0u);
                Domain const* const d = thread_domain(entry);
                running() = g_main;
                watch_reap(entry, d, blk);
                ASSERT_EQ(g_reap_last, LONG_PAGES) << "fixture: the reservation is mapped";
                size_t const doubles = host_pool_double_frees();
                exit_as(entry);
                on_window(nullptr);
                EXPECT_EQ(g_reap_windows, 3u) << "a window after each pass";
                EXPECT_EQ(g_reap_most, ASPACE_MAP_PASS_PAGES);
                EXPECT_EQ(held_pages(d, blk), 0u);
                EXPECT_EQ(host_pool_double_frees(), doubles);
                ASSERT_EQ(call_as(g_main, KOS_SYS_TASK_SLAY, h), 0u);
            }

            TEST_F(SysPresync, the_last_member_s_exit_unmaps_a_lent_reservation_and_keeps_it)
            {
                // Main's user pages handed to the lender, whose calls read and write them.
                kos_task_t const a = new_task(g_main, user_, USER_PAGES * G, 0);
                ASSERT_NE(a, KOS_TASK_NONE);
                Thread* const lender = entry_of(a);
                ASSERT_NE(lender, nullptr);
                {
                    IrqLock lock;
                    cap_seat_authority(lender, AUTH_MEMORY | AUTH_TASKS);
                }
                uintptr_t const blk = call_as(lender, KOS_SYS_RAM_ALLOC, LONG_PAGES * G);
                ASSERT_NE(blk, 0u);
                ASSERT_EQ(call_as(lender, KOS_SYS_MEM_SELF_GRANT, blk, LONG_PAGES * G, 0), 0u);
                kos_task_t b = KOS_TASK_NONE;
                ASSERT_EQ(create_task(lender, blk, LONG_PAGES * G, 0, &b, {}, EDIT_PARAMS,
                                      EDIT_OUT),
                          0);
                Thread* const borrower = entry_of(b);
                ASSERT_NE(borrower, nullptr);
                Domain const* const d = thread_domain(lender);
                ASSERT_NE(domain_ranges(d)->at_base(blk)->lends, 0u);
                running() = g_main;
                watch_span(lender, d, blk);
                ASSERT_EQ(g_span.last, LONG_PAGES) << "fixture: the lender maps its reservation";
                exit_as(lender);
                on_window(nullptr);
                // Main's pages handed to the lender take one more.
                EXPECT_EQ(g_span.windows, 4u) << "a window after each pass";
                EXPECT_EQ(g_span.most, ASPACE_MAP_PASS_PAGES);
                EXPECT_EQ(space_mapped(d, blk), 0u);
                EXPECT_EQ(create_mapped(blk), LONG_PAGES) << "the trim unmapped its borrower";
                for (uint32_t i = 0; i < LONG_PAGES; i++)
                {
                    EXPECT_TRUE(frame_taken(aspace_frame_of(blk) + i * G)) << "page " << i;
                }
            }

            TEST_F(SysPresync, the_last_window_onto_a_shed_reservation_frees_it_in_passes)
            {
                // Main's user pages handed to the lender, whose calls read and write them.
                kos_task_t const a = new_task(g_main, user_, USER_PAGES * G, 0);
                ASSERT_NE(a, KOS_TASK_NONE);
                Thread* const lender = entry_of(a);
                ASSERT_NE(lender, nullptr);
                {
                    IrqLock lock;
                    cap_seat_authority(lender, AUTH_MEMORY | AUTH_TASKS);
                }
                uintptr_t const blk = call_as(lender, KOS_SYS_RAM_ALLOC, LONG_PAGES * G);
                ASSERT_NE(blk, 0u);
                kos_task_t b = KOS_TASK_NONE;
                PeerWindow const w = {blk, LONG_PAGES * G, 0};
                ASSERT_EQ(create_task(lender, 0, 0, 0, &b, w, EDIT_PARAMS, EDIT_OUT), 0);
                Thread* const borrower = entry_of(b);
                ASSERT_NE(borrower, nullptr);
                Domain* const shed = thread_domain(lender);
                running() = g_main;
                exit_as(lender);
                ASSERT_EQ(call_as(g_main, KOS_SYS_TASK_SLAY, a), 0u);
                ASSERT_EQ(domain_space(shed), nullptr) << "fixture: the lender's space is not shed";
                ASSERT_NE(shed->ranges.at_base(blk), nullptr);
                Domain const* const d = thread_domain(borrower);
                // The window sits in the window area: only the frames count here.
                watch_reap(borrower, d, blk);
                ASSERT_EQ(g_reap_last, LONG_PAGES);
                size_t const doubles = host_pool_double_frees();
                exit_as(borrower);
                on_window(nullptr);
                EXPECT_EQ(g_reap_windows, 7u) << "a window after each pass";
                EXPECT_EQ(g_reap_most, ASPACE_MAP_PASS_PAGES);
                for (uint32_t i = 0; i < LONG_PAGES; i++)
                {
                    EXPECT_FALSE(frame_taken(aspace_frame_of(blk) + i * G)) << "page " << i;
                }
                EXPECT_EQ(host_pool_double_frees(), doubles);
            }

            // Tables from the pool, one leaf table per 32 pages: a long map's later passes link
            // tables of their own.
            class SysCreateTables : public SysPresync
            {
              protected:
                void SetUp() override
                {
                    host_aspace_tables_from_pool(true);
                    host_aspace_leaf_table_span(leaf_span_shift());
                    SysPresync::SetUp();
                }

                void TearDown() override
                {
                    SysPresync::TearDown();
                    host_aspace_leaf_table_span(0);
                    host_aspace_tables_from_pool(false);
                }

                virtual unsigned leaf_span_shift() const
                {
                    return 17u;
                }
            };

            // Two pages a leaf table: a long map's reserve takes more than one pass.
            constexpr unsigned WIDE_SPAN_SHIFT = 13u;

            class SysWideTables : public SysCreateTables
            {
              protected:
                unsigned leaf_span_shift() const override
                {
                    return WIDE_SPAN_SHIFT;
                }
            };

            // The pool frames taken, less the tables main's space links, which the host editor
            // keeps until the space goes.
            size_t loose_frames()
            {
                return host_pool_used()
                       - arch_aspace_table_pages(domain_space(thread_domain(g_main)));
            }

            uint32_t tables_reserved()
            {
                uint32_t n = 0;
                for (Task const& t : kernel().tasks())
                {
                    n = n + t.tables_reserved;
                }
                return n;
            }

            enum class Mapper
            {
                HANDOFF,
                WINDOW,
                FRAME_MAP
            };

            // What a refused call left at its windows: no window may see it built or mapped.
            struct WindowSeen
            {
                uintptr_t blk = 0;
                bool built = false;
                uint32_t mapped = 0;
            };
            WindowSeen g_seen_at_window;

            void seen_hook(uint32_t)
            {
                if (running() != g_main)
                {
                    return;
                }
                Domain* d = nullptr;
                if (g_seen_at_window.blk != 0 and building_over(g_seen_at_window.blk, &d) != nullptr)
                {
                    g_seen_at_window.built = true;
                }
                uint32_t const now = space_mapped(thread_domain(g_main), RUN_VA);
                if (now > g_seen_at_window.mapped)
                {
                    g_seen_at_window.mapped = now;
                }
            }

            class SysRefusedUpFront : public SysCreateTables,
                                      public ::testing::WithParamInterface<std::tuple<Mapper, unsigned>>
            {
              protected:
                Mapper mapper() const
                {
                    return std::get<0>(GetParam());
                }

                unsigned leaf_span_shift() const override
                {
                    return std::get<1>(GetParam());
                }

                void undo_long_map(uint32_t named, RunCaps const& run)
                {
                    if (mapper() == Mapper::HANDOFF)
                    {
                        Thread* const entry = entry_of(static_cast<kos_task_t>(named));
                        ASSERT_NE(entry, nullptr);
                        ASSERT_EQ(call_as(g_main, KOS_SYS_TASK_SLAY, named), 0u);
                        running() = entry;
                        run_exit(KOS_EXIT_CANCELLED);
                    }
                    else if (mapper() == Mapper::WINDOW)
                    {
                        running() = kernel().threads.resolve(static_cast<int>(named));
                        ASSERT_NE(running(), nullptr);
                        run_exit(0);
                    }
                    else
                    {
                        ASSERT_EQ(call_as(g_main, KOS_SYS_FRAME_UNMAP, run.fcap, run.acap, RUN_VA),
                                  0u);
                    }
                    running() = g_main;
                }
            };

            // Every pool take of the call refused in turn: the call succeeds, or is refused with
            // nothing built, nothing mapped and every frame back.
            TEST_P(SysRefusedUpFront, a_long_map_refused_a_frame_anywhere_leaves_no_trace)
            {
                uintptr_t blk = 0;
                RunCaps run;
                if (mapper() == Mapper::FRAME_MAP)
                {
                    run = mint(LONG_PAGES);
                    ASSERT_NE(run.obj, FRAME_RUN_NONE);
                }
                else
                {
                    blk = reserve_only(LONG_PAGES * G);
                    ASSERT_NE(blk, 0u);
                }
                int const tasks = created_tasks();
                bool done = false;
                for (size_t nth = 1; nth < 192 and not done; nth++)
                {
                    running() = g_main;
                    size_t const used = loose_frames();
                    sys_seam_reset();
                    g_seen_at_window = WindowSeen{blk, false, 0};
                    on_window(seen_hook);
                    host_pool_fail_in(nth);
                    int32_t rc = 0;
                    uint32_t named = 0;
                    if (mapper() == Mapper::HANDOFF)
                    {
                        kos_task_t t = KOS_TASK_NONE;
                        rc = create_task(g_main, blk, LONG_PAGES * G, 0, &t);
                        named = t;
                    }
                    else if (mapper() == Mapper::WINDOW)
                    {
                        kos_thread_t h = KOS_THREAD_NONE;
                        rc = spawn(g_main, spawn_params(EDIT_PARAMS, blk, LONG_PAGES * G, 0),
                                   MAIN_OUT, &h);
                        named = h;
                    }
                    else
                    {
                        rc = err(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, run.acap,
                                         va_word(RUN_VA), 0));
                    }
                    bool const unreached = host_pool_fail_armed();
                    host_pool_fail_in(0);
                    on_window(nullptr);
                    if (rc == 0 and unreached)
                    {
                        done = true;
                        break;
                    }
                    if (rc == 0)
                    {
                        // A round sent again took the frame: undo the call and refuse the next.
                        undo_long_map(named, run);
                        continue;
                    }
                    Domain* d = nullptr;
                    EXPECT_FALSE(g_seen_at_window.built) << "take " << nth << " refused past a pass";
                    EXPECT_EQ(g_seen_at_window.mapped, 0u)
                        << "take " << nth << " refused past a pass";
                    EXPECT_EQ(created_tasks(), tasks) << "take " << nth;
                    EXPECT_EQ(building_over(blk, &d), nullptr) << "take " << nth;
                    EXPECT_EQ(space_mapped(thread_domain(g_main), RUN_VA), 0u) << "take " << nth;
                    EXPECT_EQ(loose_frames(), used) << "take " << nth << " kept frames";
                    EXPECT_EQ(tables_reserved(), 0u) << "take " << nth;
                    EXPECT_TRUE(named == KOS_TASK_NONE or named == KOS_THREAD_NONE);
                }
                EXPECT_TRUE(done) << "fixture: no take let the call through";
                EXPECT_EQ(tables_reserved(), 0u);
                EXPECT_EQ(record_of(g_main).table_count, 0u);
            }

            INSTANTIATE_TEST_SUITE_P(
                Mappers, SysRefusedUpFront,
                ::testing::Combine(::testing::Values(Mapper::HANDOFF, Mapper::WINDOW,
                                                     Mapper::FRAME_MAP),
                                   ::testing::Values(17u, WIDE_SPAN_SHIFT)));

            TEST_F(SysCreateTables, a_creator_slain_mid_passes_gives_its_reserved_tables_back)
            {
                uintptr_t const blk = reserve_only(SHORT_PAGES * G);
                ASSERT_NE(blk, 0u);
                size_t const used = loose_frames();
                g_victim = spawn_member();
                ASSERT_NE(g_victim, nullptr);
                sys_seam_reset();
                g_slay_blk = blk;
                on_window(slay_built_hook);
                if (setjmp(g_slain) == 0)
                {
                    kos_task_t t = KOS_TASK_NONE;
                    int const rc = create_task(g_victim, blk, SHORT_PAGES * G, 0, &t, {},
                                               EDIT_PARAMS, EDIT_OUT);
                    FAIL() << "the create opened no window: " << rc;
                }
                on_window(nullptr);
                EXPECT_NE(record_of(g_victim).table_count, 0u) << "fixture: nothing reserved";
                EXPECT_NE(tables_reserved(), 0u);
                Domain* d = nullptr;
                Thread* const entry = building_over(blk, &d);
                ASSERT_NE(entry, nullptr);
                size_t const doubles = host_pool_double_frees();
                running() = g_victim;
                run_exit(KOS_EXIT_CANCELLED);
                running() = g_main;
                EXPECT_EQ(tables_reserved(), 0u);
                expect_create_gone(entry, blk, doubles);
                EXPECT_EQ(loose_frames(), used) << "a reserved table frame leaked";
            }

            TEST_F(SysCreateTables, a_mapper_slain_mid_map_gives_its_reserved_tables_back)
            {
                RunCaps const run = mint(LONG_PAGES);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                size_t const used = loose_frames();
                g_victim = spawn_member();
                ASSERT_NE(g_victim, nullptr);
                ASSERT_NE(slay_mid_frame_map(this, run, nullptr, 1, false), nullptr);
                EXPECT_NE(record_of(g_victim).table_count, 0u) << "fixture: nothing reserved";
                running() = g_victim;
                run_exit(KOS_EXIT_CANCELLED);
                running() = g_main;
                EXPECT_EQ(tables_reserved(), 0u);
                EXPECT_EQ(space_mapped(thread_domain(g_main), RUN_VA), 0u);
                EXPECT_EQ(loose_frames(), used) << "a reserved table frame leaked";
            }

            TEST_F(SysCreateTables, a_frame_map_another_map_finished_between_its_passes_is_done)
            {
                RunCaps const run = mint(LONG_PAGES);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                uint8_t const refs = run_refs(run.obj);
                g_finisher = spawn_member();
                ASSERT_NE(g_finisher, nullptr);
                g_finisher_run = share_to(g_finisher, run);
                ASSERT_NE(g_finisher_run.obj, FRAME_RUN_NONE);
                g_finisher_rc = 1;
                running() = g_main;
                size_t const used = loose_frames();
                sys_seam_reset();
                on_window(finish_hook);
                EXPECT_EQ(err(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, run.acap,
                                      va_word(RUN_VA), 0)),
                          0);
                on_window(nullptr);
                EXPECT_EQ(g_finisher_rc, 0);
                Domain const* const d = thread_domain(g_main);
                EXPECT_EQ(space_mapped(d, RUN_VA), LONG_PAGES);
                EXPECT_EQ(*host<uintptr_t>(uva(MAIN_OUT, VA_WORD_AT)), RUN_VA);
                EXPECT_EQ(record_of(g_main).table_count, 0u) << "the done map kept its reserve";
                EXPECT_EQ(tables_reserved(), 0u);
                EXPECT_EQ(loose_frames(), used) << "a reserved table frame leaked";
                EXPECT_EQ(call_as(g_main, KOS_SYS_FRAME_UNMAP, run.fcap, run.acap, RUN_VA), 0u);
                EXPECT_EQ(run_refs(run.obj), refs + 1u) << "the finished map kept a reference";
            }

            // How a thread's table reserve moved between two of its windows.
            struct TablesWatch
            {
                Thread const* t = nullptr;
                uintptr_t blk = 0;
                uint32_t last = 0;
                uint32_t most = 0;
                bool built = false;
                bool short_unmapped = false;
                bool taken_after_build = false;
                bool part_given_back = false;
            };
            TablesWatch g_tables;

            void tables_hook(uint32_t)
            {
                if (running() != g_tables.t)
                {
                    return;
                }
                uint32_t const now = record_of(running()).table_count;
                uint32_t step = now - g_tables.last;
                if (now < g_tables.last)
                {
                    step = g_tables.last - now;
                    if (now != 0)
                    {
                        g_tables.part_given_back = true;
                    }
                }
                if (step > g_tables.most)
                {
                    g_tables.most = step;
                }
                Domain* d = nullptr;
                bool const built = (g_tables.blk != 0 and building_over(g_tables.blk, &d) != nullptr)
                                   or space_mapped(thread_domain(g_main), RUN_VA) != 0;
                // The pass that takes the last tables may build: one already built may not take.
                if (now > g_tables.last and g_tables.built)
                {
                    g_tables.taken_after_build = true;
                }
                if (now > g_tables.last and not built)
                {
                    g_tables.short_unmapped = true;
                }
                g_tables.last = now;
                g_tables.built = built;
            }

            void watch_tables(Thread const* t, uintptr_t blk)
            {
                sys_seam_reset();
                g_tables = TablesWatch{};
                g_tables.t = t;
                g_tables.blk = blk;
                on_window(tables_hook);
            }

            TEST_F(SysWideTables, a_long_frame_map_takes_and_gives_back_its_tables_in_bounded_passes)
            {
                RunCaps const run = mint(LONG_PAGES);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                // Every table in place: the second map's whole reserve is left over.
                ASSERT_EQ(err(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, run.acap,
                                      va_word(RUN_VA), 0)),
                          0);
                ASSERT_EQ(call_as(g_main, KOS_SYS_FRAME_UNMAP, run.fcap, run.acap, RUN_VA), 0u);
                ASSERT_GT(arch_aspace_range_tables(RUN_VA, LONG_PAGES), ASPACE_TABLE_PASS_FRAMES)
                    << "fixture: the reserve fits one pass";
                size_t const used = loose_frames();
                watch_tables(g_main, 0);
                EXPECT_EQ(err(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, run.acap,
                                      va_word(RUN_VA), 0)),
                          0);
                on_window(nullptr);
                EXPECT_LE(g_tables.most, ASPACE_TABLE_PASS_FRAMES) << "one pass moved more tables";
                EXPECT_TRUE(g_tables.short_unmapped) << "no window inside the take";
                EXPECT_FALSE(g_tables.taken_after_build) << "a table taken once the map began";
                EXPECT_TRUE(g_tables.part_given_back) << "no window inside the give-back";
                EXPECT_EQ(space_mapped(thread_domain(g_main), RUN_VA), LONG_PAGES);
                EXPECT_EQ(record_of(g_main).table_count, 0u);
                EXPECT_EQ(tables_reserved(), 0u);
                EXPECT_EQ(loose_frames(), used) << "a reserved table frame leaked";
            }

            TEST_F(SysWideTables, a_long_window_spawn_takes_its_tables_before_it_builds_in_bounded_passes)
            {
                uintptr_t const blk = reserve_only(LONG_PAGES * G);
                ASSERT_NE(blk, 0u);
                size_t const used = loose_frames();
                watch_tables(g_main, blk);
                kos_thread_t h = KOS_THREAD_NONE;
                EXPECT_EQ(spawn(g_main, spawn_params(EDIT_PARAMS, blk, LONG_PAGES * G, 0), MAIN_OUT,
                                &h),
                          0);
                on_window(nullptr);
                EXPECT_LE(g_tables.most, ASPACE_TABLE_PASS_FRAMES) << "one pass moved more tables";
                EXPECT_TRUE(g_tables.short_unmapped) << "no window inside the take";
                EXPECT_FALSE(g_tables.taken_after_build) << "a table taken once the build began";
                EXPECT_EQ(record_of(g_main).table_count, 0u);
                EXPECT_EQ(tables_reserved(), 0u);
                running() = kernel().threads.resolve(static_cast<int>(h));
                ASSERT_NE(running(), nullptr);
                run_exit(0);
                running() = g_main;
                EXPECT_EQ(loose_frames(), used) << "a reserved table frame leaked";
            }

            TEST_F(SysWideTables, a_mapper_slain_mid_take_gives_its_tables_back_in_bounded_passes)
            {
                RunCaps const run = mint(LONG_PAGES);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                size_t const used = loose_frames();
                g_victim = spawn_member();
                ASSERT_NE(g_victim, nullptr);
                ASSERT_NE(slay_mid_frame_map(this, run, nullptr, 1, false), nullptr);
                EXPECT_EQ(record_of(g_victim).table_count, ASPACE_TABLE_PASS_FRAMES)
                    << "fixture: not slain inside the take";
                EXPECT_EQ(space_mapped(thread_domain(g_main), RUN_VA), 0u);
                watch_tables(g_victim, 0);
                g_tables.last = record_of(g_victim).table_count;
                running() = g_victim;
                run_exit(KOS_EXIT_CANCELLED);
                running() = g_main;
                on_window(nullptr);
                EXPECT_LE(g_tables.most, ASPACE_TABLE_PASS_FRAMES) << "one pass moved more tables";
                EXPECT_EQ(tables_reserved(), 0u);
                EXPECT_EQ(loose_frames(), used) << "a reserved table frame leaked";
            }

            INSTANTIATE_TEST_SUITE_P(Calls, SysPresyncSlain,
                                     ::testing::Values(Slain::SYNC, Slain::CLEAR, Slain::COPY));

            RunCaps g_cancel_run;
            kos_thread_t g_cancel_handle = KOS_THREAD_NONE;
            bool g_cancel_returned = false;

            void kill_hook(uint32_t)
            {
                PresyncRecord& r = record_of(g_victim);
                // Bounded, so a kill that never lands lets the call finish rather than spin.
                if (running() != g_victim or not r.live or g_drops_left == MAX_KILL_ROUNDS)
                {
                    return;
                }
                drop(r);
                g_drops_left++;
                if (g_drops_left == 2u)
                {
                    EXPECT_EQ(call_as(g_main, KOS_SYS_THREAD_CANCEL, g_cancel_handle,
                                      KOS_CANCEL_KILL, 0u),
                              0u);
                }
            }

            void cancel_call()
            {
                (void)call_as(g_victim, KOS_SYS_FRAME_MAP, g_cancel_run.fcap, g_cancel_run.acap,
                              va_word(RUN_VA), KOS_MEM_NOCACHE);
                g_cancel_returned = true;
                sched::exit_current(0, sched::EXIT_RETURN);
            }

            TEST_F(SysPresyncSlain, a_kill_ends_a_call_going_round_at_its_next_round)
            {
                RunCaps const run = mint(PAGES);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                g_victim = spawn_member();
                ASSERT_NE(g_victim, nullptr);
                int const slot = kernel().threads.index_of(g_victim);
                g_cancel_handle = kernel().threads.handle_for(slot);
                g_cancel_run = share(g_victim, run);
                ASSERT_NE(g_cancel_run.obj, FRAME_RUN_NONE);
                sys_seam_reset();
                g_cancel_returned = false;
                on_window(kill_hook);
                run_noreturn(cancel_call);
                on_window(nullptr);
                running() = g_main;
                EXPECT_FALSE(g_cancel_returned);
                EXPECT_EQ(g_syncs, 2u * PAGES) << "killed at the end of its second round";
                EXPECT_FALSE(record_of(g_victim).live);
                EXPECT_FALSE(record_of(g_victim).active);
                EXPECT_EQ(kernel().presync_live, 0u);

                Thread* const next = spawn_member();
                ASSERT_NE(next, nullptr);
                ASSERT_EQ(kernel().threads.index_of(next), slot);
                RunCaps const again = share(next, run);
                ASSERT_NE(again.obj, FRAME_RUN_NONE);
                sys_seam_reset();
                EXPECT_EQ(err(call_as(next, KOS_SYS_FRAME_MAP, again.fcap, again.acap,
                                      va_word(RUN_VA), KOS_MEM_NOCACHE)),
                          0);
                EXPECT_EQ(g_syncs, PAGES) << "the slot's next thread inherited a round";
            }

            TEST_F(SysPresync, a_create_whose_params_were_unreadable_at_entry_fails_without_a_round)
            {
                uintptr_t const grant = reserve_only(G);
                ASSERT_NE(grant, 0u);
                task_params(MAIN_PARAMS, grant, G, 0);
                sys_seam_reset();
                refuse_acquire(uva(MAIN_PARAMS, TASK_PARAMS_AT), 1);
                kos_task_t t = KOS_TASK_NONE;
                EXPECT_EQ(create_staged(g_main, MAIN_PARAMS, MAIN_OUT, &t), -KOS_EFAULT)
                    << "the locked pass read them again";
                EXPECT_EQ(t, KOS_TASK_NONE);
                EXPECT_EQ(g_windows, 0u);
                EXPECT_EQ(host_pool_attempts(), 0u);
            }

            TEST_F(SysPresync, a_create_unreadable_at_entry_never_takes_an_earlier_create_s_params)
            {
                kos_task_t first = KOS_TASK_NONE;
                ASSERT_EQ(create_task(g_main, 0, 0, 0, &first), 0);
                task_params(MAIN_PARAMS, 0, 0, 0);
                sys_seam_reset();
                refuse_acquire(uva(MAIN_PARAMS, TASK_PARAMS_AT), 1);
                kos_task_t t = KOS_TASK_NONE;
                EXPECT_EQ(create_staged(g_main, MAIN_PARAMS, MAIN_OUT, &t), -KOS_EFAULT)
                    << "the locked pass started a task from the last create's parameters";
                EXPECT_EQ(t, KOS_TASK_NONE);
            }

            // The create never waits: a dying instance answers the retry, and the stage the plan
            // took for it is given back with its stager mark.
            TEST_F(SysPresync, a_create_of_a_dying_instance_answers_retry_and_keeps_no_stage)
            {
                Task* record = nullptr;
                ASSERT_NE(editor_starts_a_dying_peer(&record), nullptr) << "fixture: peer0 is not dying";
                kos_task_params* const p = task_params(MAIN_PARAMS, 0, 0, 0);
                uintptr_t const name = uva(MAIN_PARAMS, 3800);
                memcpy(host<char>(name), "peer0", 6);
                p->instance = reinterpret_cast<char const*>(name);
                sys_seam_reset();
                kos_task_t t = KOS_TASK_NONE;
                EXPECT_EQ(create_staged(g_main, MAIN_PARAMS, MAIN_OUT, &t), -KOS_EAGAIN);
                EXPECT_EQ(t, KOS_TASK_NONE);
                EXPECT_EQ(g_main->wait_kind, WAIT_NONE) << "the create parked";
                EXPECT_EQ(record_of(g_main).staged, 0u) << "the refused create kept its stage";
                EXPECT_EQ(*task_image_stager(record), 0u) << "the refused create kept the stager mark";
                EXPECT_FALSE(record_of(g_main).active);
            }

            TEST_F(SysPresync, a_create_whose_window_list_was_unreadable_at_entry_fails)
            {
                uintptr_t const grant = reserve_only(G);
                uintptr_t const target = reserve_only(G);
                ASSERT_NE(grant, 0u);
                ASSERT_NE(target, 0u);
                task_params(MAIN_PARAMS, grant, G, 0, PeerWindow{target, G, 0});
                sys_seam_reset();
                refuse_acquire(uva(MAIN_PARAMS, WINDOWS_AT), 1);
                kos_task_t t = KOS_TASK_NONE;
                EXPECT_EQ(create_staged(g_main, MAIN_PARAMS, MAIN_OUT, &t), -KOS_EFAULT);
                EXPECT_EQ(t, KOS_TASK_NONE);
                EXPECT_EQ(g_syncs, 0u);
            }

            // A count past the array leaves nothing copied, so the plan must read none of the
            // record's window bytes: the slots a refused spawn left, and past them the task
            // parameters, here shaped as a sixth uncached entry on the block.
            TEST_F(SysPresync, a_create_whose_window_count_passes_the_array_answers_and_notes_nothing)
            {
                uintptr_t const block = reserve_only(G);
                ASSERT_NE(block, 0u);
                kos_task_params* const tp = task_params(MAIN_PARAMS, block, G, KOS_MEM_NOCACHE);
                kos_window const stale = {block, static_cast<uint32_t>(G), KOS_WINDOW_MEMORY,
                                          KOS_WINDOW_UNCACHED};
                constexpr size_t past = offsetof(PresyncRecord, windows)
                                        + (2u * KICKOS_MAX_THREAD_WINDOWS - 1u) * sizeof(kos_window)
                                        - offsetof(PresyncRecord, task_params);
                static_assert(offsetof(PresyncRecord, task_params)
                                      < offsetof(PresyncRecord, windows)
                                            + 2u * KICKOS_MAX_THREAD_WINDOWS * sizeof(kos_window),
                              "the arm shapes a window past the array out of the task parameters");
                static_assert(past + sizeof(kos_window) <= sizeof(kos_task_params));
                memcpy(reinterpret_cast<unsigned char*>(tp) + past, &stale, sizeof(stale));
                tp->window_count = 2u * KICKOS_MAX_THREAD_WINDOWS;
                sys_seam_reset();
                PresyncRecord& r = record_of(g_main);
                for (kos_window& w : r.windows)
                {
                    w = stale;
                }
                kos_task_t t = KOS_TASK_NONE;
                EXPECT_EQ(create_staged(g_main, MAIN_PARAMS, MAIN_OUT, &t), -KOS_ENOMEM);
                EXPECT_EQ(t, KOS_TASK_NONE);
                EXPECT_FALSE(record_of(g_main).active);
            }

            TEST_F(SysPresync, every_unprivileged_thread_enters_through_the_image_s_startup)
            {
                uintptr_t const grant = reserve_only(G);
                ASSERT_NE(grant, 0u);
                sys_seam_reset();
                kos_task_t t = KOS_TASK_NONE;
                ASSERT_EQ(create_task(g_main, grant, G, 0, &t), 0);
                EXPECT_EQ(g_context_entry, reinterpret_cast<uintptr_t>(kickos_task_start));
                EXPECT_EQ(g_second_arg, reinterpret_cast<uintptr_t>(&entry));
                uintptr_t const q = spawn_params(MAIN_PARAMS, 0, 0, 0);
                kos_thread_t child = KOS_THREAD_NONE;
                ASSERT_EQ(spawn(g_main, q, MAIN_OUT, &child), 0);
                EXPECT_EQ(g_context_entry, reinterpret_cast<uintptr_t>(kickos_task_start))
                    << "a member of a running space skipped the startup's once-guard";
                EXPECT_EQ(g_second_arg, reinterpret_cast<uintptr_t>(params(q)->entry));
            }

            TEST_F(SysPresync, a_call_refused_its_authority_or_its_out_word_stages_nothing)
            {
                uintptr_t const grant = reserve_only(G);
                ASSERT_NE(grant, 0u);
                uintptr_t const p = spawn_params(MAIN_PARAMS, 0, 0, 0);
                sys_seam_reset();
                kos_task_t t = KOS_TASK_NONE;
                cap_seat_authority(g_main, AUTH_MEMORY);
                EXPECT_EQ(create_task(g_main, grant, G, 0, &t), -KOS_EPERM);
                cap_seat_authority(g_main, AUTH_MEMORY | AUTH_TASKS);
                EXPECT_EQ(err(call_as(g_main, KOS_SYS_TASK_CREATE, program_for(g_main),
                                      uva(MAIN_PARAMS, TASK_PARAMS_AT), 0)),
                          -KOS_EINVAL);
                EXPECT_EQ(err(call_as(g_main, KOS_SYS_THREAD_CREATE, p, 0)), -KOS_EINVAL);
                EXPECT_EQ(host_pool_attempts(), 0u) << "a refused call took a run";
                EXPECT_EQ(g_windows, 0u);
            }

            void unmapped_window_list(kos_thread_params* p, uintptr_t)
            {
                p->windows = reinterpret_cast<kos_window const*>(UNMAPPED);
                p->window_count = 1;
            }

            void undefined_authority(kos_thread_params* p, uintptr_t)
            {
                p->authority = UNDEFINED_AUTHORITY;
            }

            void unknown_policy(kos_thread_params* p, uintptr_t)
            {
                p->policy = 7;
            }

            void misaligned_stack(kos_thread_params* p, uintptr_t grant)
            {
                p->stack_base = reinterpret_cast<void*>(grant + KICKOS_STACK_ALIGN / 2u);
                p->stack_size = KICKOS_USER_STACK_SIZE;
            }

            struct Malformed
            {
                char const* what;
                int32_t rc;
                void (*shape)(kos_thread_params*, uintptr_t grant);
            };

            TEST_F(SysPresync, a_spawn_its_arguments_refuse_stages_nothing)
            {
                uintptr_t const grant = reserve_only(G);
                ASSERT_NE(grant, 0u);
                Malformed const cases[] = {
                    {"window list", -KOS_EFAULT, unmapped_window_list},
                    {"authority", -KOS_EINVAL, undefined_authority},
                    {"policy", -KOS_EINVAL, unknown_policy},
                    {"stack", -KOS_EINVAL, misaligned_stack},
                };
                for (Malformed const& m : cases)
                {
                    uintptr_t const p = spawn_params(MAIN_PARAMS, 0, 0, 0);
                    m.shape(params(p), grant);
                    sys_seam_reset();
                    kos_thread_t child = KOS_THREAD_NONE;
                    EXPECT_EQ(spawn(g_main, p, MAIN_OUT, &child), m.rc) << m.what;
                    EXPECT_EQ(host_pool_attempts(), 0u) << m.what;
                }
            }

            TEST_F(SysPresync, a_create_its_arguments_refuse_leaves_nothing_staged)
            {
                uintptr_t const grant = reserve_only(G);
                ASSERT_NE(grant, 0u);
                struct
                {
                    char const* what;
                    int32_t rc;
                    uintptr_t base;
                    size_t bytes;
                    uintptr_t window;
                } const cases[] = {
                    {"window list", -KOS_EFAULT, grant, G, UNMAPPED},
                    {"data block", -KOS_EPERM, UNMAPPED, G, 0},
                    {"part of a reservation", -KOS_EPERM, grant + GRANT_BYTES / 4u,
                     GRANT_BYTES / 2u, 0},
                    {"part of a reservation and a window list", -KOS_EPERM,
                     grant + GRANT_BYTES / 4u, GRANT_BYTES / 2u, UNMAPPED},
                };
                size_t const before = host_pool_used();
                for (auto const& c : cases)
                {
                    kos_task_params* const tp = task_params(MAIN_PARAMS, c.base, c.bytes, 0);
                    if (c.window != 0)
                    {
                        tp->windows = reinterpret_cast<kos_window const*>(c.window);
                        tp->window_count = 1;
                    }
                    sys_seam_reset();
                    kos_task_t t = KOS_TASK_NONE;
                    EXPECT_EQ(create_staged(g_main, MAIN_PARAMS, MAIN_OUT, &t), c.rc) << c.what;
                    EXPECT_EQ(host_pool_used(), before) << c.what;
                }
            }

            TEST_F(SysPresync, a_stage_a_create_refused_past_its_arguments_never_took_is_freed)
            {
                uintptr_t const grant = reserve_only(G);
                ASSERT_NE(grant, 0u);
                kos_task_params* const tp = task_params(MAIN_PARAMS, grant, G, 0);
                *host<kos_cap_grant>(uva(MAIN_PARAMS, CAPS_AT)) = {NO_SUCH_CAP, KOS_CAP_SIGNAL};
                tp->caps = reinterpret_cast<kos_cap_grant const*>(uva(MAIN_PARAMS, CAPS_AT));
                tp->cap_count = 1;
                size_t const before = host_pool_used();
                sys_seam_reset();
                kos_task_t t = KOS_TASK_NONE;
                EXPECT_EQ(create_staged(g_main, MAIN_PARAMS, MAIN_OUT, &t), -KOS_EBADF);
                EXPECT_NE(host_pool_attempts(), 0u) << "the copy was staged";
                EXPECT_EQ(host_pool_used(), before);
            }

            TEST_F(SysPresync, a_pool_short_at_the_plan_alone_sends_the_create_round)
            {
                int const tasks = created_tasks();
                host_pool_fail_in(1);
                kos_task_t t = KOS_TASK_NONE;
                EXPECT_EQ(create_task(g_main, 0, 0, 0, &t), 0)
                    << "a refusal the pool no longer makes answered the create";
                EXPECT_NE(t, KOS_TASK_NONE);
                EXPECT_EQ(created_tasks(), tasks + 1);
                EXPECT_NE(g_windows, 0u) << "the round staged no copy";
                EXPECT_EQ(record_of(g_main).staged, 0u);
            }

            // Every other frame taken: no two consecutive frames for the image's data at the plan
            // nor at the locked pass, and single frames for the space's tables.
            TEST_F(SysPresync, a_pool_short_at_the_plan_and_the_locked_pass_fails_without_a_round)
            {
                ASSERT_GE(SYS_IMAGE_BYTES / G, 2u) << "fixture: the data fits one frame";
                size_t const before = host_pool_used();
                int const tasks = created_tasks();
                arch_phys_addr_t taken[HOST_POOL_FRAMES] = {};
                size_t n = 0;
                while (n < HOST_POOL_FRAMES)
                {
                    arch_phys_addr_t const pa = host_pool_take(1);
                    if (pa == 0)
                    {
                        break;
                    }
                    taken[n] = pa;
                    n++;
                }
                for (size_t i = 0; i < n; i++)
                {
                    if ((taken[i] / G) % 2u == 0u)
                    {
                        frame_pool_free_run(taken[i], 1);
                    }
                }
                size_t const fragmented = host_pool_used();
                sys_seam_reset();
                kos_task_t t = KOS_TASK_NONE;
                EXPECT_EQ(create_task(g_main, 0, 0, 0, &t), -KOS_ENOMEM);
                EXPECT_EQ(t, KOS_TASK_NONE);
                EXPECT_EQ(created_tasks(), tasks) << "a task outlived the refusal";
                EXPECT_EQ(g_windows, 0u) << "a pool still short sent the call round";
                EXPECT_EQ(host_pool_used(), fragmented);
                for (size_t i = 0; i < n; i++)
                {
                    if ((taken[i] / G) % 2u != 0u)
                    {
                        frame_pool_free_run(taken[i], 1);
                    }
                }
                EXPECT_EQ(host_pool_used(), before);
            }

            TEST_F(SysPresync, a_mint_whose_out_word_went_away_answers_efault_and_frees_its_object)
            {
                int const notifies = live_notifications();
                refuse_acquire(uva(MAIN_OUT), 1);
                EXPECT_EQ(err(call_as(g_main, KOS_SYS_NOTIFY_CREATE, uva(MAIN_OUT))), -KOS_EFAULT);
                EXPECT_EQ(live_notifications(), notifies) << "the undelivered capability stayed installed";
            }

            uintptr_t g_staged_va = 0;
            bool g_named_in_window = false;

            void clear_hook(uint32_t ordinal)
            {
                if (ordinal != 1u)
                {
                    return;
                }
                g_staged_va = aspace_user_va(record_of(g_main).staged);
                VirtualRanges const* const ranges = domain_ranges(thread_domain(g_main));
                g_named_in_window = ranges->find(g_staged_va, 1u) != nullptr;
            }

            TEST_F(SysPresync, a_reservation_is_cleared_before_the_range_names_it)
            {
                constexpr size_t CLEARED = 3;
                dirty_free_frames();
                g_staged_va = 0;
                g_named_in_window = true;
                on_window(clear_hook);
                uintptr_t const va = reserve_only(CLEARED * G);
                ASSERT_NE(va, 0u);
                EXPECT_EQ(g_staged_va, va);
                EXPECT_FALSE(g_named_in_window) << "named before its first granule was cleared";
                EXPECT_NE(domain_ranges(thread_domain(g_main))->at_base(va), nullptr);
                unsigned char const* const b = host_pool_bytes(va, CLEARED);
                size_t dirty = 0;
                for (size_t i = 0; i < CLEARED * G; i++)
                {
                    if (b[i] != 0)
                    {
                        dirty++;
                    }
                }
                EXPECT_EQ(dirty, 0u);
                EXPECT_EQ(g_windows, CLEARED) << "cleared a granule a window";
            }

            TEST_F(SysPresync, a_self_grant_syncs_only_where_its_memory_type_asks)
            {
                uintptr_t const cac = reserve_only(G);
                uintptr_t const unc = reserve_only(G);
                ASSERT_NE(cac, 0u);
                ASSERT_NE(unc, 0u);
                sys_seam_reset();
                EXPECT_EQ(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, cac, G, 0), 0u);
                EXPECT_EQ(g_syncs, 0u) << "a cacheable map";
                EXPECT_EQ(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, unc, G, KOS_MEM_NOCACHE), 0u);
                EXPECT_EQ(g_syncs, 1u) << "a non-cacheable map";
                EXPECT_EQ(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, unc, G, KOS_MEM_NOCACHE), 0u);
                EXPECT_EQ(g_syncs, 1u) << "the same grant again";
                cap_seat_authority(g_main, 0);
                EXPECT_EQ(err(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, cac, G, KOS_MEM_NOCACHE)),
                          -KOS_EPERM);
                cap_seat_authority(g_main, AUTH_MEMORY | AUTH_TASKS);
                EXPECT_EQ(g_syncs, 1u) << "a grant refused its authority";
                EXPECT_EQ(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, unc, G, 0), 0u);
                EXPECT_EQ(g_syncs, 2u) << "the retype back to cacheable";
            }

            TEST_F(SysPresync, a_frame_run_syncs_when_its_mapping_type_changes)
            {
                RunCaps const run = mint(1);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                EXPECT_EQ(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, run.acap, va_word(RUN_VA),
                                  KOS_MEM_NOCACHE),
                          0u);
                EXPECT_EQ(g_syncs, 1u) << "a non-cacheable map";
                EXPECT_EQ(err(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, run.acap,
                                      va_word(RUN_VA2), 0)),
                          -KOS_EBUSY)
                    << "one run, two types at once";
                EXPECT_EQ(call_as(g_main, KOS_SYS_FRAME_UNMAP, run.fcap, run.acap, RUN_VA), 0u);
                EXPECT_EQ(g_syncs, 1u) << "the unmap";
                EXPECT_EQ(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, run.acap,
                                  va_word(RUN_VA), 0), 0u);
                EXPECT_EQ(g_syncs, 2u) << "a cacheable map after a non-cacheable one";
                EXPECT_EQ(call_as(g_main, KOS_SYS_FRAME_UNMAP, run.fcap, run.acap, RUN_VA), 0u);
                EXPECT_EQ(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, run.acap,
                                  va_word(RUN_VA), 0), 0u);
                EXPECT_EQ(g_syncs, 2u) << "a cacheable map again";
            }

            TEST_F(SysPresync, a_self_grant_over_a_frame_capability_s_mapping_is_eperm)
            {
                RunCaps const run = mint(1);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                ASSERT_EQ(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, run.acap,
                                  va_word(RUN_VA), 0), 0u);
                EXPECT_EQ(err(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, RUN_VA, G, 0)), -KOS_EPERM)
                    << "a frame capability's mapping was answered as already granted";
                EXPECT_EQ(err(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, RUN_VA, G,
                                      KOS_MEM_NOCACHE)),
                          -KOS_EPERM);
                struct arch_aspace* const space = domain_space(thread_domain(g_main));
                bool uncached = true;
                EXPECT_NE(arch_aspace_acquire(space, RUN_VA, ARCH_ASPACE_READ, &uncached, nullptr),
                          nullptr);
                arch_aspace_release(space, RUN_VA);
                EXPECT_FALSE(uncached) << "a refused grant mapped the run again";
                EXPECT_EQ(arch_aspace_frame_at(space, RUN_VA), run.base);
                EXPECT_EQ(run_refs(run.obj), 2u);
                EXPECT_EQ(call_as(g_main, KOS_SYS_FRAME_UNMAP, run.fcap, run.acap, RUN_VA), 0u);
            }

            TEST_F(SysPresync, a_spawn_window_or_stack_over_a_frame_capability_s_mapping_is_eperm)
            {
                RunCaps const run = mint(PAGES);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                ASSERT_EQ(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, run.acap,
                                  va_word(RUN_VA), 0), 0u);
                kos_thread_t child = KOS_THREAD_NONE;
                EXPECT_EQ(spawn(g_main, spawn_params(MAIN_PARAMS, RUN_VA, PAGES * G, 0),
                                MAIN_OUT, &child),
                          -KOS_EPERM)
                    << "a spawn window over a frame capability's mapping was admitted";
                uintptr_t const p = spawn_params(MAIN_PARAMS, 0, 0, 0);
                params(p)->stack_base = reinterpret_cast<void*>(RUN_VA);
                params(p)->stack_size = PAGES * G;
                EXPECT_EQ(spawn(g_main, p, MAIN_OUT, &child), -KOS_EPERM)
                    << "a stack over a frame capability's mapping was admitted";
                EXPECT_EQ(call_as(g_main, KOS_SYS_FRAME_UNMAP, run.fcap, run.acap, RUN_VA), 0u);
            }

            constexpr uint32_t CALLER_STACK = (KICKOS_MIN_STACK_SIZE + 15u) & ~15u;
            constexpr size_t CALLER_PAGES = (CALLER_STACK + 64u + G - 1u) / G;

            TEST_F(SysPresync, a_caller_stack_in_a_reservation_has_a_kernel_alias_and_is_admitted)
            {
                uintptr_t const own = reserve(g_main, CALLER_PAGES * G, 0);
                uintptr_t const handed = reserve_only((CALLER_PAGES + 1u) * G);
                ASSERT_NE(own, 0u);
                ASSERT_NE(handed, 0u);
                struct
                {
                    char const* what;
                    uintptr_t base;
                } const cases[] = {
                    {"own reservation", own},
                    {"own reservation off its granule", own + 64u},
                };
                for (auto const& c : cases)
                {
                    EXPECT_NE(ustack_kptr(c.base), nullptr) << c.what;
                    uintptr_t const p = spawn_params(MAIN_PARAMS, 0, 0, 0);
                    params(p)->stack_base = reinterpret_cast<void*>(c.base);
                    params(p)->stack_size = CALLER_STACK;
                    kos_thread_t child = KOS_THREAD_NONE;
                    EXPECT_EQ(spawn(g_main, p, MAIN_OUT, &child), 0) << c.what;
                }
                EXPECT_NE(ustack_kptr(handed + 64u), nullptr) << "a block handed to the task";
                // The entry's argv block comes off the top of its stack.
                kos_task_params* const tp =
                    task_params(MAIN_PARAMS, handed, (CALLER_PAGES + 1u) * G, 0);
                tp->stack = reinterpret_cast<void*>(handed + 64u);
                tp->stack_size = CALLER_STACK + 128u;
                kos_task_t t = KOS_TASK_NONE;
                EXPECT_EQ(create_staged(g_main, MAIN_PARAMS, MAIN_OUT, &t), 0)
                    << "a block handed to the task joined";
            }

            // As the AMP share is in its init's space.
            TEST_F(SysPresync, a_caller_stack_no_pool_frame_backs_is_eperm_and_claims_nothing)
            {
                uintptr_t const va = RUN_VA2 + PAGES * G;
                VirtualRanges* const r = domain_ranges_mut(thread_domain(g_main));
                ASSERT_NE(r, nullptr);
                ASSERT_TRUE(r->reserve(va, CALLER_PAGES, VR_SHARE));
                ASSERT_TRUE(r->grant(va, CALLER_PAGES, ARCH_MAP_R | ARCH_MAP_W, ARCH_MAP_NORMAL));
                ASSERT_EQ(ustack_kptr(va + 64u), nullptr);
                uintptr_t const p = spawn_params(MAIN_PARAMS, 0, 0, 0);
                params(p)->stack_base = reinterpret_cast<void*>(va + 64u);
                params(p)->stack_size = CALLER_STACK;
                sys_seam_reset();
                kos_thread_t child = KOS_THREAD_NONE;
                EXPECT_EQ(spawn(g_main, p, MAIN_OUT, &child), -KOS_EPERM);
                EXPECT_EQ(child, KOS_THREAD_NONE);
                EXPECT_EQ(host_pool_attempts(), 0u);
                EXPECT_TRUE(r->release(va));
            }

            TEST_F(SysPresync, a_wide_block_syncs_each_granule_once_per_mapping_call)
            {
                constexpr uint32_t WIDE = 32;
                uintptr_t const wide = reserve_only(WIDE * G);
                ASSERT_NE(wide, 0u);
                sys_seam_reset();
                EXPECT_EQ(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, wide, WIDE * G,
                                  KOS_MEM_NOCACHE),
                          0u);
                EXPECT_EQ(g_syncs, WIDE) << "self-grant";
                kos_thread_t child = KOS_THREAD_NONE;
                EXPECT_EQ(spawn(g_main, window_spawn(wide, WIDE * G), MAIN_OUT, &child), 0);
                EXPECT_EQ(g_syncs, 2u * WIDE) << "window";
                EXPECT_NE(new_task(g_main, wide, WIDE * G, KOS_MEM_NOCACHE), KOS_TASK_NONE);
                EXPECT_EQ(g_syncs, 3u * WIDE) << "task data";
                EXPECT_GE(g_windows, g_syncs) << "a window per granule synced";
            }

            TEST_F(SysPresync, a_non_cacheable_mapping_torn_down_leaves_the_next_cacheable_a_sync)
            {
                constexpr uint32_t TD = 3;
                uintptr_t const held = reserve_only(TD * G);
                uintptr_t const data = reserve_only(TD * G);
                ASSERT_NE(held, 0u);
                ASSERT_NE(data, 0u);
                kos_task_t const t = new_task(g_main, 0, 0, 0,
                                              PeerWindow{held, TD * G, KOS_WINDOW_UNCACHED});
                ASSERT_NE(t, KOS_TASK_NONE);
                kos_task_t const d = new_task(g_main, data, TD * G, KOS_MEM_NOCACHE);
                ASSERT_NE(d, KOS_TASK_NONE);
                sys_seam_reset();
                Thread* const h = entry_of(t);
                ASSERT_NE(h, nullptr);
                exit_as(h);
                EXPECT_EQ(call_as(g_main, KOS_SYS_TASK_SLAY, t), 0u);
                exit_as(entry_of(d));
                EXPECT_EQ(call_as(g_main, KOS_SYS_TASK_SLAY, d), 0u);
                EXPECT_EQ(g_syncs, 0u) << "a teardown syncs nothing of its own";
                EXPECT_EQ(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, held, TD * G, 0), 0u);
                EXPECT_EQ(g_syncs, TD) << "after a window";
                EXPECT_EQ(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, data, TD * G, 0), 0u);
                EXPECT_EQ(g_syncs, 2u * TD) << "after a task's data";
                kos_task_t const later = new_task(g_main, 0, 0, 0, PeerWindow{held, TD * G, 0});
                ASSERT_NE(later, KOS_TASK_NONE);
                EXPECT_EQ(g_syncs, 2u * TD) << "a second cacheable mapping";
                exit_as(entry_of(later));
                EXPECT_EQ(call_as(g_main, KOS_SYS_TASK_SLAY, later), 0u);
                EXPECT_EQ(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, held, TD * G, KOS_MEM_NOCACHE),
                          0u);
                EXPECT_EQ(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, held, TD * G, 0), 0u);
                EXPECT_EQ(g_syncs, 4u * TD) << "a retype round trip";
            }

            // How the copy staged for peer1 leaves its stager.
            enum class Unstage
            {
                BEGIN,   // a call entering with it still held
                RELEASE, // the refused call's release
                EXIT     // the stager's exit
            };

            constexpr uint32_t WATCH_BIT = 5;

            class SysPresyncWatch : public SysPresync
            {
              protected:
                // The editor's create holds a copy of peer1's data staged, and main arms an
                // INSTANCE watch on peer1 raising WATCH_BIT of a notification of its own.
                void watch_while_staged()
                {
                    running() = g_editor;
                    {
                        IrqLock lock;
                        presync_stage_image(1);
                    }
                    running() = g_main;
                    ASSERT_NE(*task_image_stager(task_decl_record(1)), 0u) << "fixture: nothing staged";
                    ASSERT_EQ(notify_create(g_main, &note_), 0);
                    uint32_t copy = KCAP_INVALID;
                    ASSERT_EQ(notify_badge(g_main, note_, WATCH_BIT, &copy), 0);
                    memcpy(host<char>(uva(EDIT_PARAMS, PROGRAM_AT)), "peer1", 6);
                    *host<uint32_t>(uva(EDIT_PARAMS, PROGRAM_AT + 8)) = KCAP_INVALID;
                    ASSERT_EQ(err(call_as(g_main, KOS_SYS_WATCH,
                                          watch_spec(KOS_WATCH_INSTANCE, KOS_INSTANCE_STARTABLE),
                                          program_for(g_main), copy, uva(EDIT_PARAMS, PROGRAM_AT),
                                          uva(EDIT_PARAMS, PROGRAM_AT + 8))),
                              0)
                        << "a copy another create holds staged was not in the way";
                    ASSERT_EQ(pending(), 0u);
                }

                uint32_t pending() const
                {
                    IrqLock lock;
                    int e = 0;
                    Notification const* const n = static_cast<Notification*>(
                        cap_resolve_e(g_main, note_, CapType::CAP_NOTIFY, 0, &e));
                    if (n == nullptr)
                    {
                        return 0xDEADu;
                    }
                    return n->pending;
                }

                // A look taken inside the stager's call, as a doorbell pass interrupting it takes
                // one, then the copy leaving it by `how`.
                void unstage(Unstage how)
                {
                    running() = g_editor;
                    start_watch_poke();
                    poked_inside_ = pending();
                    if (how == Unstage::BEGIN)
                    {
                        IrqLock lock;
                        (void)presync_begin();
                    }
                    else if (how == Unstage::RELEASE)
                    {
                        presync_release(false);
                    }
                    else
                    {
                        IrqLock lock;
                        presync_exit();
                    }
                    running() = g_main;
                }

                uint32_t note_ = KCAP_INVALID;
                uint32_t poked_inside_ = 0xDEADu;
            };

            TEST_F(SysPresyncWatch, a_call_entering_with_the_copy_staged_raises_the_watch_it_held)
            {
                watch_while_staged();
                unstage(Unstage::BEGIN);
                EXPECT_EQ(poked_inside_, 0u) << "a look inside the stager's call took its copy as no one's";
                EXPECT_EQ(*task_image_stager(task_decl_record(1)), 0u);
                EXPECT_EQ(pending(), 1u << WATCH_BIT) << "the copy left and the watch was not raised";
            }

            TEST_F(SysPresyncWatch, a_refused_call_releasing_the_copy_raises_the_watch_it_held)
            {
                watch_while_staged();
                unstage(Unstage::RELEASE);
                EXPECT_EQ(poked_inside_, 0u) << "a look inside the stager's call took its copy as no one's";
                EXPECT_EQ(*task_image_stager(task_decl_record(1)), 0u);
                EXPECT_EQ(pending(), 1u << WATCH_BIT) << "the copy left and the watch was not raised";
            }

            TEST_F(SysPresyncWatch, the_stager_s_exit_dropping_the_copy_raises_the_watch_it_held)
            {
                watch_while_staged();
                unstage(Unstage::EXIT);
                EXPECT_EQ(poked_inside_, 0u) << "a look inside the stager's call took its copy as no one's";
                EXPECT_EQ(*task_image_stager(task_decl_record(1)), 0u);
                EXPECT_EQ(pending(), 1u << WATCH_BIT) << "the copy left and the watch was not raised";
            }
        }
    }
}
