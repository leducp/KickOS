// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A system call's work outside the kernel lock, over the REAL kernel/syscall/syscall.cc and
// syscall_thread.cc on the translating posture: the round the dispatch sends a call through
// again when its record drops, what another thread's call completed in an interrupt window
// does to it, what a slain or a refused call leaves behind, and the syncs each mapping call
// owes. The window is the seam's: a case acts there as the thread an interrupt would wake.

#include <kickos/sync.h>

#include <setjmp.h>

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
                return kernel().presync[kernel().threads.index_of(t)];
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

            int live_semaphores()
            {
                int n = 0;
                for (uint8_t const r : kernel().sem_refs)
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
                for (Task const& t : kernel().tasks)
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
                        case Edit::WINDOW:
                        case Edit::FAULTED_OUT:
                        case Edit::FAILED_SPAWN:
                        {
                            kos_task_t const t = new_task(g_editor, 0, 0, 0);
                            uintptr_t const p = spawn_params(EDIT_PARAMS, blk_, bytes_,
                                                             KOS_WINDOW_UNCACHED, t);
                            if (edit_ == Edit::FAILED_SPAWN)
                            {
                                // A destination past the child's table fails the spawn after
                                // its window is mapped.
                                uintptr_t const caps = uva(EDIT_PARAMS, CAPS_AT);
                                uintptr_t const dest = uva(EDIT_PARAMS, DEST_AT);
                                *host<kos_cap_grant>(caps) = {ed_run_.acap, KOS_CAP_TRANSFER};
                                *host<uint16_t>(dest) = PAST_CHILD_TABLE;
                                params(p)->caps = reinterpret_cast<kos_cap_grant const*>(caps);
                                params(p)->cap_dest = reinterpret_cast<uint16_t const*>(dest);
                                params(p)->cap_count = 1;
                            }
                            if (edit_ == Edit::FAULTED_OUT)
                            {
                                refuse_acquire(uva(EDIT_OUT), 1);
                            }
                            kos_thread_t child = KOS_THREAD_NONE;
                            g_seen.rc = spawn(g_editor, p, EDIT_OUT, &child);
                            g_seen.live_after = main_record.live;
                            break;
                        }
                        case Edit::HANDOFF:
                        {
                            kos_task_t t = KOS_TASK_NONE;
                            g_seen.rc = create_task(g_editor, blk_, bytes_, KOS_MEM_NOCACHE, &t);
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
                            (void)call_as(g_editor, KOS_SYS_THREAD_SELF);
                            g_seen.rc = 0;
                            g_seen.live_after = main_record.live;
                            break;
                        }
                        case Edit::FLIP:
                        {
                            kos_thread_params* const p = params(uva(MAIN_PARAMS));
                            p->task = KOS_TASK_NONE;
                            p->mem_base = reinterpret_cast<void*>(blk_);
                            p->mem_size = static_cast<uint32_t>(bytes_);
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
                    g_case->edit();
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
                kos_task_t const into = new_task(g_main, 0, 0, 0);
                ASSERT_NE(blk, 0u);
                ASSERT_NE(into, KOS_TASK_NONE);
                drop_records();
                kos_thread_t child = KOS_THREAD_NONE;
                EXPECT_EQ(spawn(g_main, window_spawn(blk, PAGES * G, into), MAIN_OUT, &child), 0);
                EXPECT_NE(child, KOS_THREAD_NONE);
                expect_rounds_dropped(PAGES);
            }

            TEST_F(SysPresync, a_privileged_caller_s_call_opens_no_window)
            {
                RunCaps const run = mint(PAGES);
                ASSERT_NE(run.obj, FRAME_RUN_NONE);
                g_main->privileged = true;
                g_drops_left = DROPS;
                on_window(drop_hook);
                EXPECT_EQ(err(call_as(g_main, KOS_SYS_FRAME_MAP, run.fcap, run.acap,
                                      va_word(RUN_VA), KOS_MEM_NOCACHE)),
                          0);
                EXPECT_EQ(g_syncs, PAGES) << "only the first round, which no window dropped";
                EXPECT_EQ(g_windows, 0u);
                g_main->privileged = false;
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
                kos_task_t const into = new_task(g_main, 0, 0, 0);
                ASSERT_NE(blk, 0u);
                ASSERT_NE(into, KOS_TASK_NONE);
                arm(Edit::HANDOFF, blk, PAGES * G);
                kos_thread_t child = KOS_THREAD_NONE;
                EXPECT_EQ(spawn(g_main, window_spawn(blk, PAGES * G, into), MAIN_OUT, &child), 0);
                expect_seen(0, false, 1, "the editor's completed handoff dropped it");
            }

            TEST_F(SysPresync, a_spawn_whose_handle_is_lost_still_sends_the_call_round)
            {
                uintptr_t const blk = reserve_only(PAGES * G);
                ASSERT_NE(blk, 0u);
                arm(Edit::FAULTED_OUT, blk, PAGES * G);
                EXPECT_NE(new_task(g_main, blk, PAGES * G, KOS_MEM_NOCACHE), KOS_TASK_NONE);
                expect_seen(-KOS_EFAULT, false, 1,
                            "its out-word went away after the check, and its mapping stands");
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
                uintptr_t const grant = reserve_only(G);
                kos_task_t const into = new_task(g_main, 0, 0, 0);
                ASSERT_NE(blk, 0u);
                ASSERT_NE(grant, 0u);
                ASSERT_NE(into, KOS_TASK_NONE);
                uintptr_t const p = window_spawn(blk, PAGES * G, into);
                arm(Edit::FLIP, grant, G);
                kos_thread_t child = KOS_THREAD_NONE;
                EXPECT_EQ(spawn(g_main, p, MAIN_OUT, &child), 0);
                EXPECT_TRUE(g_seen.ran);
                EXPECT_EQ(rounds(), 0u);
                Thread const* const t = kernel().threads.resolve(child);
                ASSERT_NE(t, nullptr);
                EXPECT_EQ(t->task, task_resolve(into)) << "the child joined the task first named";
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
              protected:
                Thread* spawn_member()
                {
                    uintptr_t const p = spawn_params(EDIT_PARAMS, 0, 0, 0, KOS_TASK_NONE);
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
                    EXPECT_EQ(call_as(g_main, KOS_SYS_THREAD_KILL, g_cancel_handle), 0u);
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

            TEST_F(SysPresync, a_spawn_whose_params_were_unreadable_at_entry_fails_without_a_round)
            {
                uintptr_t const grant = reserve_only(G);
                ASSERT_NE(grant, 0u);
                uintptr_t const p = building_spawn(grant);
                sys_seam_reset();
                refuse_acquire(p, 1);
                kos_thread_t child = KOS_THREAD_NONE;
                EXPECT_EQ(spawn(g_main, p, MAIN_OUT, &child), -KOS_EFAULT)
                    << "the locked pass read them again";
                EXPECT_EQ(child, KOS_THREAD_NONE);
                EXPECT_EQ(g_windows, 0u);
                EXPECT_EQ(host_pool_attempts(), 0u);
            }

            TEST_F(SysPresync, a_spawn_whose_window_list_was_unreadable_at_entry_fails)
            {
                uintptr_t const grant = reserve_only(G);
                uintptr_t const target = reserve_only(G);
                ASSERT_NE(grant, 0u);
                ASSERT_NE(target, 0u);
                uintptr_t const p = building_spawn(grant);
                uintptr_t const w = uva(MAIN_WINDOWS);
                *host<kos_window>(w) = {target, static_cast<uint32_t>(G), KOS_WINDOW_MEMORY, 0};
                params(p)->windows = reinterpret_cast<kos_window const*>(w);
                params(p)->window_count = 1;
                sys_seam_reset();
                refuse_acquire(w, 1);
                kos_thread_t child = KOS_THREAD_NONE;
                EXPECT_EQ(spawn(g_main, p, MAIN_OUT, &child), -KOS_EFAULT);
                EXPECT_EQ(child, KOS_THREAD_NONE);
                EXPECT_EQ(g_syncs, 0u);
            }

            TEST_F(SysPresync, a_call_refused_its_authority_or_its_out_word_stages_nothing)
            {
                uintptr_t const grant = reserve_only(G);
                ASSERT_NE(grant, 0u);
                uintptr_t const p = building_spawn(grant);
                sys_seam_reset();
                kos_thread_t child = KOS_THREAD_NONE;
                kos_task_t t = KOS_TASK_NONE;
                cap_seat_authority(g_main, AUTH_MEMORY);
                EXPECT_EQ(create_task(g_main, 0, 0, 0, &t), -KOS_EPERM);
                EXPECT_EQ(spawn(g_main, p, MAIN_OUT, &child), -KOS_EPERM);
                cap_seat_authority(g_main, AUTH_MEMORY | AUTH_TASKS);
                EXPECT_EQ(err(call_as(g_main, KOS_SYS_TASK_CREATE, 0, 0, 0, 0)), -KOS_EINVAL);
                EXPECT_EQ(err(call_as(g_main, KOS_SYS_THREAD_CREATE, p, 0)), -KOS_EINVAL);
                EXPECT_EQ(host_pool_attempts(), 0u) << "a refused call took a run";
                EXPECT_EQ(g_windows, 0u);
            }

            void unmapped_window_list(kos_thread_params* p, uintptr_t)
            {
                p->windows = reinterpret_cast<kos_window const*>(UNMAPPED);
                p->window_count = 1;
            }

            void unmapped_grant(kos_thread_params* p, uintptr_t)
            {
                p->mem_base = reinterpret_cast<void*>(UNMAPPED);
            }

            void undefined_authority(kos_thread_params* p, uintptr_t)
            {
                p->authority = UNDEFINED_AUTHORITY;
            }

            void misaligned_stack(kos_thread_params* p, uintptr_t grant)
            {
                p->stack_base = reinterpret_cast<void*>(grant + KICKOS_STACK_ALIGN / 2u);
                p->stack_size = KICKOS_USER_STACK_SIZE;
            }

            void part_of_the_grant(kos_thread_params* p, uintptr_t grant)
            {
                p->mem_base = reinterpret_cast<void*>(grant + GRANT_BYTES / 4u);
                p->mem_size = GRANT_BYTES / 2u;
            }

            void part_of_the_grant_and_unmapped_window_list(kos_thread_params* p, uintptr_t grant)
            {
                part_of_the_grant(p, grant);
                unmapped_window_list(p, grant);
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
                    {"data grant", -KOS_EPERM, unmapped_grant},
                    {"authority", -KOS_EINVAL, undefined_authority},
                    {"stack", -KOS_EINVAL, misaligned_stack},
                    {"part of a reservation", -KOS_EPERM, part_of_the_grant},
                    {"part of a reservation and a window list", -KOS_EFAULT,
                     part_of_the_grant_and_unmapped_window_list},
                };
                for (Malformed const& m : cases)
                {
                    uintptr_t const p = building_spawn(grant);
                    m.shape(params(p), grant);
                    sys_seam_reset();
                    kos_thread_t child = KOS_THREAD_NONE;
                    EXPECT_EQ(spawn(g_main, p, MAIN_OUT, &child), m.rc) << m.what;
                    EXPECT_EQ(host_pool_attempts(), 0u) << m.what;
                }
            }

            TEST_F(SysPresync, a_stage_a_spawn_refused_past_its_arguments_never_took_is_freed)
            {
                uintptr_t const grant = reserve_only(G);
                ASSERT_NE(grant, 0u);
                uintptr_t const p = building_spawn(grant);
                *host<kos_cap_grant>(uva(MAIN_PARAMS, CAPS_AT)) = {NO_SUCH_CAP, KOS_CAP_SIGNAL};
                params(p)->caps = reinterpret_cast<kos_cap_grant const*>(uva(MAIN_PARAMS, CAPS_AT));
                params(p)->cap_count = 1;
                size_t const before = host_pool_used();
                sys_seam_reset();
                kos_thread_t child = KOS_THREAD_NONE;
                EXPECT_EQ(spawn(g_main, p, MAIN_OUT, &child), -KOS_EBADF);
                EXPECT_NE(host_pool_attempts(), 0u) << "the copy was staged";
                EXPECT_EQ(host_pool_used(), before);
            }

            TEST_F(SysPresync, a_pool_refusing_the_stage_fails_task_create_without_a_round)
            {
                size_t const before = host_pool_used();
                int const tasks = created_tasks();
                host_pool_fail_in(1);
                kos_task_t t = KOS_TASK_NONE;
                EXPECT_EQ(create_task(g_main, 0, 0, 0, &t), -KOS_ENOMEM);
                EXPECT_EQ(t, KOS_TASK_NONE);
                EXPECT_EQ(created_tasks(), tasks) << "a task outlived the refusal";
                EXPECT_EQ(g_windows, 0u) << "a resource refusal is not a round";
                EXPECT_EQ(host_pool_used(), before);
            }

            TEST_F(SysPresync, a_mint_whose_out_word_went_away_answers_efault_and_keeps_its_object)
            {
                int const sems = live_semaphores();
                refuse_acquire(uva(MAIN_OUT), 1);
                EXPECT_EQ(err(call_as(g_main, KOS_SYS_SEM_CREATE, 0, uva(MAIN_OUT))), -KOS_EFAULT);
                EXPECT_EQ(live_semaphores(), sems + 1) << "the capability stays installed";
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
                EXPECT_NE(arch_aspace_acquire(space, RUN_VA, &uncached), nullptr);
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
                EXPECT_EQ(spawn(g_main, spawn_params(MAIN_PARAMS, RUN_VA, PAGES * G, 0,
                                                     KOS_TASK_NONE),
                                MAIN_OUT, &child),
                          -KOS_EPERM)
                    << "a spawn window over a frame capability's mapping was admitted";
                uintptr_t const p = spawn_params(MAIN_PARAMS, 0, 0, 0, KOS_TASK_NONE);
                params(p)->stack_base = reinterpret_cast<void*>(RUN_VA);
                params(p)->stack_size = PAGES * G;
                EXPECT_EQ(spawn(g_main, p, MAIN_OUT, &child), -KOS_EPERM)
                    << "a stack over a frame capability's mapping was admitted";
                EXPECT_EQ(call_as(g_main, KOS_SYS_FRAME_UNMAP, run.fcap, run.acap, RUN_VA), 0u);
            }

            TEST_F(SysPresync, a_wide_block_syncs_each_granule_once_per_mapping_call)
            {
                constexpr uint32_t WIDE = 32;
                uintptr_t const wide = reserve_only(WIDE * G);
                kos_task_t const into = new_task(g_main, 0, 0, 0);
                ASSERT_NE(wide, 0u);
                ASSERT_NE(into, KOS_TASK_NONE);
                sys_seam_reset();
                EXPECT_EQ(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, wide, WIDE * G,
                                  KOS_MEM_NOCACHE),
                          0u);
                EXPECT_EQ(g_syncs, WIDE) << "self-grant";
                kos_thread_t child = KOS_THREAD_NONE;
                EXPECT_EQ(spawn(g_main, window_spawn(wide, WIDE * G, into), MAIN_OUT, &child), 0);
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
                kos_task_t const t = new_task(g_main, 0, 0, 0);
                kos_task_t const later = new_task(g_main, 0, 0, 0);
                ASSERT_NE(held, 0u);
                ASSERT_NE(data, 0u);
                ASSERT_NE(t, KOS_TASK_NONE);
                ASSERT_NE(later, KOS_TASK_NONE);
                kos_thread_t holder = KOS_THREAD_NONE;
                ASSERT_EQ(spawn(g_main, window_spawn(held, TD * G, t), MAIN_OUT, &holder), 0);
                kos_task_t const d = new_task(g_main, data, TD * G, KOS_MEM_NOCACHE);
                ASSERT_NE(d, KOS_TASK_NONE);
                sys_seam_reset();
                Thread* const h = kernel().threads.resolve(holder);
                ASSERT_NE(h, nullptr);
                exit_as(h);
                EXPECT_EQ(call_as(g_main, KOS_SYS_TASK_KILL, t), 0u);
                EXPECT_EQ(call_as(g_main, KOS_SYS_TASK_KILL, d), 0u);
                EXPECT_EQ(g_syncs, 0u) << "a teardown syncs nothing of its own";
                EXPECT_EQ(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, held, TD * G, 0), 0u);
                EXPECT_EQ(g_syncs, TD) << "after a window";
                EXPECT_EQ(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, data, TD * G, 0), 0u);
                EXPECT_EQ(g_syncs, 2u * TD) << "after a task's data";
                kos_thread_t second = KOS_THREAD_NONE;
                EXPECT_EQ(spawn(g_main, spawn_params(MAIN_PARAMS, held, TD * G, 0, later),
                                MAIN_OUT, &second),
                          0);
                EXPECT_EQ(g_syncs, 2u * TD) << "a second cacheable mapping";
                Thread* const s2 = kernel().threads.resolve(second);
                ASSERT_NE(s2, nullptr);
                exit_as(s2);
                EXPECT_EQ(call_as(g_main, KOS_SYS_TASK_KILL, later), 0u);
                EXPECT_EQ(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, held, TD * G, KOS_MEM_NOCACHE),
                          0u);
                EXPECT_EQ(call_as(g_main, KOS_SYS_MEM_SELF_GRANT, held, TD * G, 0), 0u);
                EXPECT_EQ(g_syncs, 4u * TD) << "a retype round trip";
            }
        }
    }
}
