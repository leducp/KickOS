// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The per-task object budget. THE CLAIM THAT NEEDS TWO TASKS is the whole point of the
// mechanism: one task at its ceiling must not be able to refuse ANOTHER task's creator. An
// arm that exhausts a POOL proves nothing about it, so every arm below asserts the pool still
// has free slots at the moment the refusal lands.
//
// Host-only, and the fixture is why: the four creators charge Thread::task, so the claim is
// about two REAL Task slots with real threads in them, and the sim's own selftest reaches only
// one task at a time. sem_create/mutex_create come from syscall_obj.cc, which this gate's
// library compiles and kickos_kseam does not; the endpoint creator's refusal is a board arm
// (user/apps/common/objbudget), the whole far-IPC path sitting behind syscall_ipc.cc.
//
// THE CEILING HAS TWO HALVES and the posture (CMakeLists.txt) separates them: the semaphore
// pool is wide, so KICKOS_TASK_OBJECT_BUDGET is what refuses; the mutex pool is two slots, so
// TASK_OBJECT_RESERVE is. Neither half can be dropped without reddening an arm here.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

#include "syscall_internal.h"

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class TaskBudget : public KSeam
            {
            };

            static_assert(KICKOS_TASK_OBJECT_BUDGET == 2,
                          "these arms count creates against the budget by hand; the posture "
                          "in this directory's CMakeLists.txt is what sets it");
            static_assert(KICKOS_MAX_SEMAPHORES > KICKOS_TASK_OBJECT_BUDGET
                                                      + TASK_OBJECT_RESERVE,
                          "the semaphore arms must leave the pool slots to spare at the "
                          "refusal, or they would be measuring an exhausted pool");
            static_assert(KICKOS_MAX_MUTEXES - TASK_OBJECT_RESERVE < KICKOS_TASK_OBJECT_BUDGET,
                          "the mutex arm must be refused by the pool RESERVE and not by the "
                          "budget, or dropping the reserve would redden nothing");

            constexpr int SLOT_FIRST = 0;
            constexpr int SLOT_SECOND = 1;
            constexpr int SLOT_THIRD = 2;

            constexpr uint8_t PRIO_MID = 5;
            // Two past the ceiling plus the reserved plane, so the refusal an arm reads is
            // never -KOS_EMFILE and the slab still backs a run for every thread an arm seats.
            constexpr uint32_t CAP_WIDTH =
                KICKOS_CAP_FIRST_DYNAMIC + KICKOS_TASK_OBJECT_BUDGET + 2;

            // Any in-range line does: no arm here ever arms one.
            constexpr int LINE_A = 11;

            // A thread of its own task, current, with a table it can mint into. `slot` is a
            // POOL slot, because task_release and the exit sweep both walk kernel().threads.
            Thread* creator_in_task(int slot, int task_slot)
            {
                Task* const group = task(task_slot);
                Thread* const t = seat_pool(slot, PRIO_MID);
                join_task(t, group);
                attach_caps(t, CAP_WIDTH);
                kernel().current[kickos_kernel_core()] = t;
                return t;
            }

            int live_sems()
            {
                int n = 0;
                for (int i = 0; i < KICKOS_MAX_SEMAPHORES; i++)
                {
                    if (kernel().sems.live(i))
                    {
                        n++;
                    }
                }
                return n;
            }

            int live_mutexes()
            {
                int n = 0;
                for (int i = 0; i < KICKOS_MAX_MUTEXES; i++)
                {
                    if (kernel().mutexes.live(i))
                    {
                        n++;
                    }
                }
                return n;
            }

            // Fill `c`'s task to its semaphore ceiling and hand back the last cap.
            uint32_t fill_to_the_ceiling(Thread* c)
            {
                uint32_t cap = KCAP_INVALID;
                for (int i = 0; i < KICKOS_TASK_OBJECT_BUDGET; i++)
                {
                    kernel().current[kickos_kernel_core()] = c;
                    EXPECT_EQ(sem_create(0, &cap), 0)
                        << "fixture: create " << i << " is inside the ceiling";
                }
                return cap;
            }

            // THE REFUSAL, AND ITS OWN CODE. -KOS_ENOMEM would say the pool could not
            // allocate and -KOS_EMFILE that this thread's table refused the mint; the pool has
            // slots and the table has slots, so neither is what happened.
            TEST_F(TaskBudget, a_task_at_its_budget_is_refused_while_its_pool_has_slots)
            {
                Thread* const c = creator_in_task(SLOT_FIRST, 0);
                (void)fill_to_the_ceiling(c);
                ASSERT_EQ(live_sems(), KICKOS_TASK_OBJECT_BUDGET);

                uint32_t cap = KCAP_INVALID;
                kernel().current[kickos_kernel_core()] = c;
                EXPECT_EQ(sem_create(0, &cap), -KOS_EOVERFLOW)
                    << "past its ceiling a task is refused, and with the budget's own code";
                EXPECT_EQ(cap, KCAP_INVALID) << "a refused create discloses no capability";
                EXPECT_EQ(live_sems(), KICKOS_TASK_OBJECT_BUDGET)
                    << "and spends no pool slot on the way to the refusal";
                EXPECT_GT(KICKOS_MAX_SEMAPHORES - live_sems(), TASK_OBJECT_RESERVE)
                    << "the pool still has slots to spare, so this was not exhaustion";
            }

            // THE HALF THE BUDGET CANNOT EXPRESS. Two slots and a reserve of one, so the
            // ceiling is a single mutex even though the budget would allow two: a pool no
            // wider than the budget is where the structural reserve is the only thing keeping
            // a slot for anybody else.
            TEST_F(TaskBudget, the_pool_reserve_keeps_a_slot_a_task_cannot_reach)
            {
                Thread* const c = creator_in_task(SLOT_FIRST, 0);
                uint32_t cap = KCAP_INVALID;
                ASSERT_EQ(mutex_create(&cap), 0);
                ASSERT_EQ(live_mutexes(), 1);

                kernel().current[kickos_kernel_core()] = c;
                EXPECT_EQ(mutex_create(&cap), -KOS_EOVERFLOW)
                    << "the reserve refuses below the budget where the pool is the narrower "
                       "of the two";
                EXPECT_EQ(live_mutexes(), 1) << "and the pool's last slot is still free";
            }

            // THE ITEM'S OWN CLAIM, and it takes two tasks to state: the supervisor whose
            // respawn must still get an object is a DIFFERENT task from the one that ran the
            // creator in a loop.
            TEST_F(TaskBudget, a_second_tasks_creator_lands_while_the_first_is_at_its_ceiling)
            {
                Thread* const first = creator_in_task(SLOT_FIRST, 0);
                (void)fill_to_the_ceiling(first);
                uint32_t refused = KCAP_INVALID;
                kernel().current[kickos_kernel_core()] = first;
                ASSERT_EQ(sem_create(0, &refused), -KOS_EOVERFLOW);

                Thread* const second = creator_in_task(SLOT_SECOND, 1);
                ASSERT_NE(second->task, first->task) << "fixture: two distinct groups";

                uint32_t cap = KCAP_INVALID;
                EXPECT_EQ(sem_create(0, &cap), 0)
                    << "one task at its ceiling must not deny another task's creator";
                EXPECT_NE(cap, KCAP_INVALID);
                // And the second task has its OWN whole ceiling, not what the first left.
                kernel().current[kickos_kernel_core()] = second;
                EXPECT_EQ(sem_create(0, &cap), 0) << "the ceiling is per task, not shared";
                kernel().current[kickos_kernel_core()] = second;
                EXPECT_EQ(sem_create(0, &cap), -KOS_EOVERFLOW)
                    << "and the second task is bounded by the same ceiling";
            }

            // THE HALF A CREATE-ONLY ARM CANNOT SEE. A budget that is spent and never
            // returned kills a long-running task by its own churn, so the loop runs well past
            // the ceiling and every iteration must land.
            TEST_F(TaskBudget, a_create_and_destroy_loop_does_not_spend_the_budget)
            {
                Thread* const c = creator_in_task(SLOT_FIRST, 0);
                for (int i = 0; i < KICKOS_MAX_SEMAPHORES * 2; i++)
                {
                    uint32_t cap = KCAP_INVALID;
                    kernel().current[kickos_kernel_core()] = c;
                    ASSERT_EQ(sem_create(0, &cap), 0)
                        << "iteration " << i << " was refused, so a release did not give the "
                                                "budget back";
                    ASSERT_EQ(handle_close(c, cap), 0);
                }
                EXPECT_EQ(live_sems(), 0) << "the loop leaves the pool as it found it";
            }

            // The same claim for the fourth creator, whose release is on its own path:
            // irq_ref_drop detaches the line before the slot returns, and above one kernel
            // core the slot comes back later still from a reclamation.
            TEST_F(TaskBudget, a_claim_and_release_loop_does_not_spend_the_budget)
            {
                Thread* const c = creator_in_task(SLOT_FIRST, 0);
                for (int i = 0; i < KICKOS_MAX_IRQ_HANDLES * 2; i++)
                {
                    uint32_t cap = KCAP_INVALID;
                    ASSERT_EQ(irq_claim(c, LINE_A, 0, &cap), 0)
                        << "claim " << i << " was refused, so a release did not give the "
                                            "budget back";
                    ASSERT_EQ(handle_close(c, cap), 0);
                }
            }

            // A DEAD TASK'S TAG MUST NOT BE INHERITED. An object outlives its creator on
            // somebody else's capability; the surviving tag names a SLOT, and a task seated in
            // that slot afterwards would be charged for objects it never created and refused
            // its own ceiling.
            TEST_F(TaskBudget, an_object_outliving_its_creator_is_charged_to_nobody)
            {
                // Task slot order is the fixture's: free_slot() scans upward, so the group
                // that has to DIE and come back must be the lower slot.
                Thread* const doomed = creator_in_task(SLOT_FIRST, 0);
                Task* const doomed_task = doomed->task;
                Thread* const holder = creator_in_task(SLOT_SECOND, 1);

                uint32_t mine = KCAP_INVALID;
                kernel().current[kickos_kernel_core()] = doomed;
                ASSERT_EQ(sem_create(0, &mine), 0);
                // A second holder in another task, taken the way a delegation does, so the
                // object survives the creator's exit.
                CapEntry const* const e = cap_lookup(doomed, mine);
                ASSERT_NE(e, nullptr);
                int const obj = e->obj;
                {
                    IrqLock lock;
                    ASSERT_TRUE(obj_ref_inc(CapType::CAP_SEM, obj, 0));
                    uint32_t theirs = KCAP_INVALID;
                    ASSERT_EQ(cap_install(holder, obj, CapType::CAP_SEM, 0, &theirs), 0);
                }

                kernel().current[kickos_kernel_core()] = doomed;
                run_exit(0);
                ASSERT_EQ(task_member_count(doomed_task), 0) << "fixture: the group emptied";
                ASSERT_EQ(live_sems(), 1) << "fixture: the object outlived its creator";
                // The fixture mints with a tag naming no pool slot, so the exit above orphans
                // nothing: this is the drop task_orphan_created_by would have made, and it is
                // what frees the slot.
                task_drop_hold(doomed_task);

                // A new task into the slot the dead one held, and it must get its whole
                // ceiling rather than the survivor's leftovers.
                Thread* const fresh = creator_in_task(SLOT_THIRD, 0);
                ASSERT_EQ(fresh->task, doomed_task) << "fixture: the slot came back";
                uint32_t cap = KCAP_INVALID;
                for (int i = 0; i < KICKOS_TASK_OBJECT_BUDGET; i++)
                {
                    kernel().current[kickos_kernel_core()] = fresh;
                    EXPECT_EQ(sem_create(0, &cap), 0)
                        << "create " << i << " was refused, so the recycled slot inherited a "
                                             "dead task's charge";
                }
            }
        }
    }
}
