// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A dying member vacates its address space BEFORE it drops the reference that can destroy it.
//
// The invariant: a core's translation base names the space of the thread that core is
// running, or the boot root; and a thread that has passed task_release in exit_current is
// running on the boot root. Therefore a space installed anywhere implies a live member, which
// implies a positive refcount, which is the negation of the destroy precondition. Without it,
// aspace_release's peer sweep clears a peer's BOOKKEEPING cell and cannot reach that peer's
// translation register, so the peer keeps walking tables arch_aspace_destroy frees. On rv64
// the kernel's own top-level entries live in the root page that gets freed.
//
// The placement is what carries it, and both halves are asserted: AFTER ustack_free, whose
// maintenance is paid against the installed space, and BEFORE task_release.
//
// WHAT THIS GATE CANNOT WITNESS, stated as a limit and not as an omission: that a real satp
// or TTBR0 write retires the walker's cached root, and that the freed root frame is never
// walked afterwards. No board the host arch answers for translates at all, so the layer under
// kernel/mem is a seam here (aspace_seam.cc). Those two are the target's, on qemu-riscv64-smp
// and qemu-arm64-smp.

#include <string.h>

#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include "aspace_seam.h"
#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class LeaveSpace : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    aspace_seam_reset();
                }
            };

            // Not slot 0: that one is root's, and this gate is about membership rather than
            // about ROOT_INDEX.
            constexpr int SLOT_BYSTANDER = 0;
            constexpr int SLOT_DYING = 1;
            constexpr int SLOT_PEER = 2;
            // The fixture's idle TCB carries no affinity mask, so it is placeable on no core:
            // a two-core arm must leave one READY thread per core behind, or the second death
            // reaches pick_next with nothing to answer.
            constexpr int SLOT_SPARE = 3;

            constexpr uint8_t PRIO_LOW = 4;
            constexpr uint8_t PRIO_MID = 5;

            // A pretend stack run: ustack_free is a seam here, so only the address travels.
            constexpr uintptr_t STACK_BASE = 0x40000000u;
            constexpr size_t STACK_BYTES = 0x4000u;

            // Where `token` sits in the trace, or -1. The trace carries switch and gap tokens
            // this gate makes no claim about, so the arms compare POSITIONS rather than the
            // whole string.
            int position_of(char const* token)
            {
                char const* const found = strstr(trace(), token);
                if (found == nullptr)
                {
                    return -1;
                }
                return static_cast<int>(found - trace());
            }

            Thread* member_of(int slot, uint8_t prio, Task* group)
            {
                Thread* const t = seat_pool(slot, prio);
                join_task(t, group);
                return t;
            }

            // The kernel took this thread's run, so exit_current owes it a ustack_free.
            void own_stack(Thread* t)
            {
                t->kstack_owned = true;
                t->stack_base = reinterpret_cast<void*>(STACK_BASE);
                t->stack_size = STACK_BYTES;
            }

            void nothing() {}

            // One bit per core holding g_watch_space, sampled inside the capability sweep.
            // The sampler runs as a gap action, which takes no argument, so both travel here.
            constexpr uint32_t HOLDERS_UNREAD = 0xFFFFFFFFu;
            struct arch_aspace* g_watch_space = nullptr;
            uint32_t g_gap_holders = HOLDERS_UNREAD;

            void sample_holders()
            {
                uint32_t holders = 0;
                for (uint32_t c = 0; c < KICKOS_NUM_CORES; c++)
                {
                    if (installed_on(c) == g_watch_space)
                    {
                        holders |= 1u << c;
                    }
                }
                g_gap_holders = holders;
            }

            // RUNNING as well as current: a peer's pick_next refuses another core's running
            // thread, and a member left merely READY would be picked up by the core whose own
            // member just died, which reinstalls the space legitimately.
            void run_as(uint32_t core, Thread* t)
            {
                g_core = core;
                kernel().current[core] = t;
                t->state = ThreadState::RUNNING;
                install_here(space_of(task_domain(t->task)));
            }
        }

        // THE ORDER, which is the whole of the fix. ustack_free first, because the page
        // maintenance it pays is against the root this core still holds; install_boot second;
        // task_release last, because from that call the space may be destroyed under this core.
        TEST_F(LeaveSpace, a_dying_member_leaves_its_space_before_it_leaves_its_task)
        {
            (void)seat_pool(SLOT_BYSTANDER, PRIO_LOW);
            Task* const group = task(0);
            ASSERT_NE(group, nullptr);
            Thread* const dying = member_of(SLOT_DYING, PRIO_MID, group);
            own_stack(dying);

            run_as(0, dying);
            ASSERT_EQ(installed_on(0), space_of(task_domain(group)));
            trace_reset();
            run_exit(0);

            int const freed = position_of("ustack_free");
            int const installed = position_of("install_boot");
            int const released = position_of("task_release");
            ASSERT_GE(freed, 0) << "the arm did not reach ustack_free: trace " << trace();
            ASSERT_GE(installed, 0)
                << "no install_boot in " << trace()
                << ": the dying member never left the space it was running in";
            ASSERT_GE(released, 0) << "the arm did not reach task_release: trace " << trace();
            EXPECT_LT(freed, installed)
                << "the install must FOLLOW ustack_free, whose maintenance is paid against "
                   "the installed space: trace "
                << trace();
            EXPECT_LT(installed, released)
                << "and PRECEDE task_release, past which the space may be destroyed: trace "
                << trace();
            EXPECT_GE(position_of("ustack_free(seated)"), 0)
                << "ustack_free ran with the space still installed on this core, which is "
                   "what its page maintenance is paid against: trace "
                << trace();
            EXPECT_EQ(g_ustack_frees, 1u);
            EXPECT_EQ(g_ustack_free_base, STACK_BASE);
        }

        // THE PEER'S VIEW, which is the crash the audit names. One member per core in one
        // space, and the reading is taken INSIDE the capability sweep: that is the window the
        // hazard lives in: task_release has already run, the sweep drops IrqLock between
        // chunks, and a peer's release is free to reach arch_aspace_destroy right there.
        // Reading after the exit instead would prove nothing, the switch away installing the
        // incoming thread's space in any case.
        TEST_F(LeaveSpace, no_core_holds_the_root_of_a_task_that_has_lost_its_last_member)
        {
            (void)seat_pool(SLOT_BYSTANDER, PRIO_LOW);
            (void)seat_pool(SLOT_SPARE, PRIO_LOW);
            Task* const group = task(0);
            ASSERT_NE(group, nullptr);
            Thread* const first = member_of(SLOT_DYING, PRIO_MID, group);
            Thread* const second = member_of(SLOT_PEER, PRIO_MID, group);
            own_stack(first);
            own_stack(second);
            attach_caps(first, KICKOS_CAP_CHILD_WIDTH);
            attach_caps(second, KICKOS_CAP_CHILD_WIDTH);

            struct arch_aspace* const space = space_of(task_domain(group));
            g_watch_space = space;
            run_as(1, second);
            run_as(0, first);
            ASSERT_EQ(installed_on(0), space);
            ASSERT_EQ(installed_on(1), space);

            g_gap_holders = HOLDERS_UNREAD;
            run_in_chunk_gap(sample_holders, 1u);
            run_exit(0);
            ASSERT_NE(g_gap_holders, HOLDERS_UNREAD)
                << "the sweep opened no chunk gap, so the arm read nothing";
            EXPECT_EQ(g_gap_holders, 0x2u)
                << "past task_release the dying member's own core must be off the space, and "
                   "the peer running a LIVE member must be untouched";
            ASSERT_EQ(task_member_count(group), 1);

            run_as(1, second);
            g_gap_holders = HOLDERS_UNREAD;
            run_in_chunk_gap(sample_holders, 1u);
            run_exit(0);
            EXPECT_EQ(task_member_count(group), 0)
                << "the group is empty, so the destroy precondition holds";
            ASSERT_NE(g_gap_holders, HOLDERS_UNREAD);
            EXPECT_EQ(g_gap_holders, 0u)
                << "and no core held the root arch_aspace_destroy is entitled to free";
        }

        // The spaceless case: the install is unconditional, so a thread whose task holds no
        // space reaches task_release on the boot root exactly as a member does, and its exit
        // changes nothing observable.
        TEST_F(LeaveSpace, a_thread_holding_no_space_still_leaves_on_the_boot_root)
        {
            (void)seat_pool(SLOT_BYSTANDER, PRIO_LOW);
            Thread* const dying = seat_pool(SLOT_DYING, PRIO_MID);
            ASSERT_EQ(dying->task, nullptr);

            g_core = 0;
            kernel().current[0] = dying;
            install_here(boot_space());
            trace_reset();
            run_exit(0);

            EXPECT_EQ(g_ustack_frees, 0u)
                << "no run of its own, so nothing to give back";
            EXPECT_EQ(installed_on(0), boot_space());
        }

        // `dying` is set before the install, and the install is not conditional on it: a
        // thread preempted inside its own capability teardown has already left its space, so
        // the restart guard elsewhere cannot put it back on one.
        TEST_F(LeaveSpace, the_install_happens_before_the_preemptible_sweep)
        {
            (void)seat_pool(SLOT_BYSTANDER, PRIO_LOW);
            Task* const group = task(0);
            ASSERT_NE(group, nullptr);
            Thread* const dying = member_of(SLOT_DYING, PRIO_MID, group);
            own_stack(dying);
            attach_caps(dying, KICKOS_CAP_CHILD_WIDTH);

            run_as(0, dying);
            trace_reset();
            // An ordinal no chunk boundary reaches: the arm wants the gaps TRACED, and no
            // action taken in one.
            run_in_chunk_gap(nothing, 1000u);
            run_exit(0);

            int const installed = position_of("install_boot");
            int const first_gap = position_of("gap");
            ASSERT_GE(installed, 0);
            ASSERT_GE(first_gap, 0) << "the sweep opened no chunk gap: trace " << trace();
            EXPECT_LT(installed, first_gap)
                << "the sweep is preemptible, and it must not run with a space installed that "
                   "the peer's release may already have destroyed: trace "
                << trace();
        }
    }
}
