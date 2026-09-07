// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Root's pool slot is RETIRED rather than reclaimed, and what that retirement owes.
//
// Root may reach EXITED: a fault, a group cancel and wild-stack containment all route root
// into sched::exit_current, and only its ORDINARY exit is turned into a shutdown
// (KOS_SYS_EXIT, init-return-is-shutdown). So the pool's `+1` slot cannot rest on root never
// dying, and what it rests on instead is ThreadPool::alloc never handing ROOT_INDEX out
// again. Three things ride on that: the is_root identity three syscalls read, the
// index-derived kill tag root's children still name, and root's KICKOS_ROOT_STACK_SIZE block,
// which the reclaim-point harvest would push onto a KICKOS_USER_STACK_SIZE free list.
//
// A slot that is never reclaimed also never reaches the reclaim point, which is the ONLY
// caller of thread_cap_release: the last arm here is about the directory that would otherwise
// be stranded, and it reddens if that branch in exit_current is dropped.
//
// Host-only, single core: every claim is pool arithmetic, and the pool is not per-core.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
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
            class RootRetire : public KSeam
            {
            };

            constexpr int SLOT_ROOT = ThreadPool::ROOT_INDEX;
            constexpr int SLOT_CHILD = 1;
            constexpr int SLOT_BYSTANDER = 2;

            constexpr uint8_t PRIO_LOW = 4;
            constexpr uint8_t PRIO_MID = 5;

            // The root slot's occupant, and a live thread beside it so the death does not take
            // the last-thread-out path (karch_seam.cc's kickos_terminate).
            Thread* seat_root()
            {
                (void)seat_pool(SLOT_BYSTANDER, PRIO_LOW);
                return seat_pool(SLOT_ROOT, PRIO_MID);
            }

            // alloc leaves `state` to thread_create, so an arm claiming twice must take the
            // slot out of EXITED itself or the second scan matches the first slot again.
            int claim()
            {
                int index = -1;
                {
                    IrqLock lock;
                    index = kernel().threads.alloc();
                }
                if (index >= 0)
                {
                    kernel().threads.slots[index].state = ThreadState::READY;
                }
                return index;
            }

            void kill(Thread* t)
            {
                kernel().current[kickos_kernel_core()] = t;
                run_exit(0);
            }

            // Runs of KICKOS_MAX_HANDLES the slab still hands out, counted by taking them all.
            // Destructive, so an arm calls it last.
            uint32_t runs_left()
            {
                uint32_t taken = 0;
                while (true)
                {
                    CapRun run{};
                    uint16_t head = 0;
                    uint16_t width = 0;
                    bool got = false;
                    {
                        IrqLock lock;
                        got = cap_slab_attach(&run, KICKOS_MAX_HANDLES, &head, &width);
                    }
                    if (not got)
                    {
                        return taken;
                    }
                    taken++;
                }
            }
        }

        // THE SKIP is the first statement of the scan, upstream of the EXITED key, so there is
        // no window for a race against it.
        TEST_F(RootRetire, a_dead_root_does_not_give_its_slot_back)
        {
            Thread* const root = seat_root();
            ASSERT_TRUE(kernel().threads.is_root(root));

            kill(root);
            ASSERT_EQ(root->state, ThreadState::EXITED);

            int const next = claim();
            EXPECT_GE(next, 0) << "the pool still has slots; the skip is not a refusal";
            EXPECT_NE(next, ThreadPool::ROOT_INDEX)
                << "root's slot is retired: reclaiming it hands root's identity, root's kill "
                   "tag and root's oversized stack block to a stranger";
        }

        // The identity half, which is what syscall.cc, syscall_thread.cc and syscall_amp.cc
        // read. All three want the IDENTITY, and none of them wants the slot.
        TEST_F(RootRetire, root_keeps_its_identity_and_no_successor_gains_it)
        {
            Thread* const root = seat_root();
            kill(root);

            int const next = claim();
            ASSERT_GE(next, 0);
            EXPECT_TRUE(kernel().threads.is_root(root))
                << "the dead occupant of the root slot is still root";
            EXPECT_FALSE(kernel().threads.is_root(&kernel().threads.slots[next]))
                << "and the thread the next spawn took is not";
        }

        // The consequence section 1.3 of the design turns from a corruption into a standing:
        // the sweep never runs for root's tag, so root's children keep it, and nobody else can
        // ever answer it.
        TEST_F(RootRetire, roots_children_keep_their_spawner_tag)
        {
            Thread* const root = seat_root();
            uint16_t const root_tag = kernel().threads.kill_tag_of(root);
            Thread* const child = seat_pool(SLOT_CHILD, PRIO_LOW);
            child->spawner_tag = root_tag;

            kill(root);
            int const next = claim();
            ASSERT_GE(next, 0);

            EXPECT_EQ(child->spawner_tag, root_tag)
                << "the reclaim sweep that clears an orphan's tag never runs for root's";
            EXPECT_NE(kernel().threads.kill_tag_of(&kernel().threads.slots[next]), root_tag)
                << "and no successor answers that tag, so none inherits cancel authority";
        }

        // THE RETIRE WORK. ThreadPool::alloc is the only caller of thread_cap_release, so a
        // slot it never reclaims needs the directory returned somewhere else. Reddens both
        // without the skip (the directory goes back, but so does the slot) and without the
        // branch exit_current grew for it.
        TEST_F(RootRetire, roots_capability_directory_goes_back_to_the_slab)
        {
            Thread* const root = seat_root();
            attach_caps(root, KICKOS_MAX_HANDLES);
            ASSERT_TRUE(cap_run_held(root->caps));

            kill(root);

            EXPECT_FALSE(cap_run_held(root->caps))
                << "root's run is the widest in the system, and no reclaim point comes for it";
        }

        // The same claim measured against the slab rather than the TCB: the chunks are
        // available again, which a cleared directory pointer alone does not prove.
        TEST_F(RootRetire, the_slab_can_hand_roots_chunks_out_again)
        {
            uint32_t const fresh = runs_left();
            ASSERT_GT(fresh, 1u) << "the arm below needs a slab that holds more than one run";

            reset();
            Thread* const root = seat_root();
            attach_caps(root, KICKOS_MAX_HANDLES);
            kill(root);

            EXPECT_EQ(runs_left(), fresh)
                << "a stranded directory would leave the slab one run short for the rest of "
                   "the system's life";
        }

        // The control: the skip is not a hole. An ordinary slot still reclaims
        // lowest-exited-first, with slot 0 dead and skipped over.
        TEST_F(RootRetire, an_ordinary_slot_still_reclaims_lowest_exited_first)
        {
            Thread* const root = seat_root();
            Thread* const low = seat_pool(3, PRIO_LOW);
            Thread* const high = seat_pool(4, PRIO_LOW);
            kill(root);
            kill(low);
            kill(high);

            EXPECT_EQ(claim(), 3) << "the lowest EXITED slot above ROOT_INDEX";
            EXPECT_EQ(claim(), 4) << "then the next one up";
        }
    }
}
