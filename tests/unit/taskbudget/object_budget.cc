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
// THE CEILING IS ONE FIGURE PER POOL KIND and the posture (CMakeLists.txt) sets three of them
// apart, so an arm reads its own pool's budget and a ceiling collapsed into one fleet figure
// reddens here. Every pool is left wider than its budget, which is what lets an arm tell a
// budget refusal from an exhausted pool.
//
// WHAT A TASK HOLDS IS DERIVED FROM ITS MEMBERS' CAPABILITY TABLES and recorded nowhere, so
// the arms below are written against HOLDS and not against creates: a delegated capability,
// a second name for one object, and a sibling thread's create are each a case the accounting
// gets wrong under a different plausible shape of the same fix.

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

            static_assert(KICKOS_TASK_SEMAPHORE_BUDGET == 2,
                          "these arms count creates against the budget by hand; the posture "
                          "in this directory's CMakeLists.txt is what sets it");
            static_assert(KICKOS_MAX_SEMAPHORES > KICKOS_TASK_SEMAPHORE_BUDGET + 1,
                          "the semaphore arms must leave the pool slots to spare at the "
                          "refusal, or they would be measuring an exhausted pool");
            static_assert(KICKOS_TASK_MUTEX_BUDGET < KICKOS_TASK_SEMAPHORE_BUDGET,
                          "the mutex arm must sit at a DIFFERENT ceiling from the semaphore "
                          "arms, or one figure standing for all four pools would redden "
                          "nothing here");
            static_assert(KICKOS_MAX_MUTEXES > KICKOS_TASK_MUTEX_BUDGET + 1,
                          "and the mutex pool must keep slots free at that refusal, or the "
                          "arm would be measuring an exhausted pool");
            static_assert(KICKOS_TASK_ENDPOINT_BUDGET == 2,
                          "the recycled-slot arm counts two endpoint holds by hand");
            static_assert(KICKOS_TASK_IRQ_HANDLE_BUDGET == 3,
                          "the IRQ arm counts claims by hand, and its ceiling must be a THIRD "
                          "figure: at the semaphore or endpoint budget an IRQ ceiling reading "
                          "either of those pools would refuse in exactly the same place");
            static_assert(KICKOS_TASK_IRQ_HANDLE_BUDGET != KICKOS_TASK_SEMAPHORE_BUDGET
                              and KICKOS_TASK_IRQ_HANDLE_BUDGET != KICKOS_TASK_MUTEX_BUDGET
                              and KICKOS_TASK_IRQ_HANDLE_BUDGET != KICKOS_TASK_ENDPOINT_BUDGET,
                          "and it must differ from every other budget in this posture, or the "
                          "IRQ arm passes for a kind literal naming the wrong pool");
            static_assert(KICKOS_MAX_IRQ_HANDLES > KICKOS_TASK_IRQ_HANDLE_BUDGET + 1,
                          "the IRQ arm must leave binding slots free at its refusal, or it "
                          "would be measuring an exhausted pool");

            constexpr int SLOT_FIRST = 0;
            constexpr int SLOT_SECOND = 1;
            constexpr int SLOT_THIRD = 2;

            constexpr uint8_t PRIO_MID = 5;
            // Four past the ceiling plus the reserved plane, so the refusal an arm reads is
            // never -KOS_EMFILE and the slab still backs a run for every thread an arm seats.
            // FOUR and not two: an arm holding a full ceiling of DELEGATED capabilities then
            // creates against it, and both sets sit in one table.
            constexpr uint32_t CAP_WIDTH =
                KICKOS_CAP_FIRST_DYNAMIC + KICKOS_TASK_SEMAPHORE_BUDGET + 4;

            // Any in-range line does: no arm here ever arms one. FOUR of them, because a
            // line carries ONE driver: a task holding several bindings at once needs a line
            // each, and the IRQ ceiling arm holds its whole ceiling and then asks for one
            // more.
            constexpr int LINE_A = 11;
            constexpr int LINE_B = 12;
            constexpr int LINE_C = 13;
            constexpr int LINE_D = 14;
            constexpr int IRQ_LINES[] = {LINE_A, LINE_B, LINE_C, LINE_D};
            static_assert(static_cast<int>(sizeof(IRQ_LINES) / sizeof(IRQ_LINES[0]))
                              > KICKOS_TASK_IRQ_HANDLE_BUDGET,
                          "the IRQ ceiling arm needs a line for every claim inside the "
                          "ceiling and one more for the claim that must be refused");

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

            // A SECOND thread of an EXISTING group. task() mints a group and refuses to
            // land off its named slot, so a sibling cannot come through creator_in_task.
            Thread* sibling_of(Thread* member, int slot)
            {
                Thread* const t = seat_pool(slot, PRIO_MID);
                join_task(t, member->task);
                attach_caps(t, CAP_WIDTH);
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

            int live_irq_bindings()
            {
                int n = 0;
                for (int i = 0; i < KICKOS_MAX_IRQ_HANDLES; i++)
                {
                    if (kernel().irq_bindings.live(i))
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

            // Fill `c`'s task to its semaphore ceiling, recording each capability where the
            // caller asks for them.
            void fill_to_the_ceiling(Thread* c, uint32_t* out)
            {
                for (int i = 0; i < KICKOS_TASK_SEMAPHORE_BUDGET; i++)
                {
                    uint32_t cap = KCAP_INVALID;
                    kernel().current[kickos_kernel_core()] = c;
                    EXPECT_EQ(sem_create(0, &cap), 0)
                        << "fixture: create " << i << " is inside the ceiling";
                    if (out != nullptr)
                    {
                        out[i] = cap;
                    }
                }
            }

            // Copy `from`'s capability `cap` into `to`'s table, exactly as a spawn grant
            // takes one: obj_ref_inc then cap_install, charging nothing anywhere.
            uint32_t delegate(Thread* from, uint32_t cap, Thread* to)
            {
                CapEntry const* const e = cap_lookup(from, cap);
                EXPECT_NE(e, nullptr) << "fixture: the source capability must resolve";
                if (e == nullptr)
                {
                    return KCAP_INVALID;
                }
                int const obj = e->obj;
                uint32_t theirs = KCAP_INVALID;
                IrqLock lock;
                EXPECT_TRUE(obj_ref_inc(CapType::CAP_SEM, obj, 0));
                EXPECT_EQ(cap_install(to, obj, CapType::CAP_SEM, 0, &theirs), 0);
                return theirs;
            }

            // The object handle behind a capability, for a grant list.
            int object_of(Thread* t, uint32_t cap)
            {
                CapEntry const* const e = cap_lookup(t, cap);
                EXPECT_NE(e, nullptr) << "fixture: the capability must resolve";
                if (e == nullptr)
                {
                    return 0;
                }
                return e->obj;
            }

            // THE REFUSAL, AND ITS OWN CODE. -KOS_ENOMEM would say the pool could not
            // allocate and -KOS_EMFILE that this thread's table refused the mint; the pool has
            // slots and the table has slots, so neither is what happened.
            TEST_F(TaskBudget, a_task_at_its_budget_is_refused_while_its_pool_has_slots)
            {
                Thread* const c = creator_in_task(SLOT_FIRST, 0);
                fill_to_the_ceiling(c, nullptr);
                ASSERT_EQ(live_sems(), KICKOS_TASK_SEMAPHORE_BUDGET);

                uint32_t cap = KCAP_INVALID;
                kernel().current[kickos_kernel_core()] = c;
                EXPECT_EQ(sem_create(0, &cap), -KOS_EOVERFLOW)
                    << "past its ceiling a task is refused, and with the budget's own code";
                EXPECT_EQ(cap, KCAP_INVALID) << "a refused create discloses no capability";
                EXPECT_EQ(live_sems(), KICKOS_TASK_SEMAPHORE_BUDGET)
                    << "and spends no pool slot on the way to the refusal";
                EXPECT_GT(KICKOS_MAX_SEMAPHORES - live_sems(), 0)
                    << "the pool still has slots to spare, so this was not exhaustion";
            }

            // THE FOURTH CREATOR'S OWN REFUSAL. Without this arm three kind literals pass the
            // whole suite: irq_claim naming CAP_SEM instead of CAP_IRQ at its admission, and
            // task_object_ceiling's CAP_IRQ case answering any other budget. Both are
            // refusals this arm reads directly, the IRQ ceiling being
            // a THIRD figure in this posture (3, against 2 and 1), so a literal naming
            // another pool moves where the refusal lands.
            TEST_F(TaskBudget, the_irq_ceiling_refuses_while_the_binding_pool_has_slots)
            {
                Thread* const c = creator_in_task(SLOT_FIRST, 0);
                uint32_t caps[KICKOS_TASK_IRQ_HANDLE_BUDGET];
                for (int i = 0; i < KICKOS_TASK_IRQ_HANDLE_BUDGET; i++)
                {
                    caps[i] = KCAP_INVALID;
                    ASSERT_EQ(irq_claim(c, IRQ_LINES[i], 0, &caps[i]), 0)
                        << "claim " << i << " is inside the ceiling and must land; a ceiling "
                                            "reading another pool's budget refuses it here";
                    ASSERT_NE(caps[i], KCAP_INVALID);
                }
                ASSERT_EQ(live_irq_bindings(), KICKOS_TASK_IRQ_HANDLE_BUDGET);

                uint32_t refused = KCAP_INVALID;
                EXPECT_EQ(irq_claim(c, IRQ_LINES[KICKOS_TASK_IRQ_HANDLE_BUDGET], 0, &refused),
                          -KOS_EOVERFLOW)
                    << "past its ceiling the fourth creator is refused, and with the budget's "
                       "own code; an admission asked about another kind admits this";
                EXPECT_EQ(refused, KCAP_INVALID) << "a refused claim discloses no capability";
                EXPECT_EQ(live_irq_bindings(), KICKOS_TASK_IRQ_HANDLE_BUDGET)
                    << "and spends no binding slot on the way to the refusal";
                EXPECT_GT(KICKOS_MAX_IRQ_HANDLES - live_irq_bindings(), 0)
                    << "the pool still has slots to spare, so this was not exhaustion";
            }

            // AND A SECOND TASK STILL CLAIMS, over the pool the first is holding its whole
            // ceiling of. The same claim the semaphore arms make, on the one creator that
            // had no arm making it.
            TEST_F(TaskBudget, a_second_tasks_claim_lands_while_the_first_is_at_its_ceiling)
            {
                Thread* const first = creator_in_task(SLOT_FIRST, 0);
                for (int i = 0; i < KICKOS_TASK_IRQ_HANDLE_BUDGET; i++)
                {
                    uint32_t cap = KCAP_INVALID;
                    ASSERT_EQ(irq_claim(first, IRQ_LINES[i], 0, &cap), 0);
                }
                uint32_t refused = KCAP_INVALID;
                ASSERT_EQ(irq_claim(first, IRQ_LINES[KICKOS_TASK_IRQ_HANDLE_BUDGET], 0,
                                    &refused),
                          -KOS_EOVERFLOW);

                Thread* const second = creator_in_task(SLOT_SECOND, 1);
                ASSERT_NE(second->task, first->task) << "fixture: two distinct groups";
                uint32_t cap = KCAP_INVALID;
                EXPECT_EQ(irq_claim(second, IRQ_LINES[KICKOS_TASK_IRQ_HANDLE_BUDGET], 0, &cap),
                          0)
                    << "one task at its IRQ ceiling must not deny another task's claim";
                EXPECT_NE(cap, KCAP_INVALID);
            }

            // A POOL OF ITS OWN, AT A CEILING OF ITS OWN. The mutex budget is one where the
            // semaphore budget is two, over a mutex pool three slots wider than that, so the
            // refusal below is the mutex figure and cannot be an exhausted pool. A ceiling
            // that read one fleet-wide figure, or the semaphore one, reddens here.
            TEST_F(TaskBudget, each_pool_kind_carries_its_own_ceiling)
            {
                Thread* const c = creator_in_task(SLOT_FIRST, 0);
                uint32_t cap = KCAP_INVALID;
                ASSERT_EQ(mutex_create(&cap), 0);
                ASSERT_EQ(live_mutexes(), 1);

                kernel().current[kickos_kernel_core()] = c;
                EXPECT_EQ(mutex_create(&cap), -KOS_EOVERFLOW)
                    << "the mutex budget refuses at one while the semaphore budget is two";
                EXPECT_EQ(live_mutexes(), 1);
                EXPECT_GT(KICKOS_MAX_MUTEXES - live_mutexes(), 0)
                    << "the pool still has slots to spare, so this was not exhaustion";
            }

            // THE ITEM'S OWN CLAIM, and it takes two tasks to state: the supervisor whose
            // respawn must still get an object is a DIFFERENT task from the one that ran the
            // creator in a loop.
            TEST_F(TaskBudget, a_second_tasks_creator_lands_while_the_first_is_at_its_ceiling)
            {
                Thread* const first = creator_in_task(SLOT_FIRST, 0);
                fill_to_the_ceiling(first, nullptr);
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

            // THE ITEM'S OWN DEFECT. A capability delegated into another task is that
            // task's HOLD, so its ceiling counts it. The creator keeps its own hold too: a
            // delegation is a copy and both tables name the slot.
            TEST_F(TaskBudget, a_delegated_object_counts_against_the_task_that_holds_it)
            {
                Thread* const maker = creator_in_task(SLOT_FIRST, 0);
                uint32_t mine[KICKOS_TASK_SEMAPHORE_BUDGET];
                fill_to_the_ceiling(maker, mine);
                Thread* const holder = creator_in_task(SLOT_SECOND, 1);

                uint32_t cap = KCAP_INVALID;
                kernel().current[kickos_kernel_core()] = holder;
                ASSERT_EQ(sem_create(0, &cap), 0) << "fixture: the second task starts empty";
                ASSERT_EQ(handle_close(holder, cap), 0);

                for (int i = 0; i < KICKOS_TASK_SEMAPHORE_BUDGET; i++)
                {
                    ASSERT_NE(delegate(maker, mine[i], holder), KCAP_INVALID)
                        << "fixture: delegation " << i;
                }

                kernel().current[kickos_kernel_core()] = holder;
                EXPECT_EQ(sem_create(0, &cap), -KOS_EOVERFLOW)
                    << "a task holding a ceiling of DELEGATED objects is at its ceiling";
                EXPECT_GT(KICKOS_MAX_SEMAPHORES - live_sems(), 0)
                    << "the pool still has slots to spare, so this was not exhaustion";
            }

            // THE ROUTE THE ITEM NAMES, END TO END: the creator dies and the survivor is
            // still bounded by what it holds. A charge recorded against the CREATOR is
            // cleared when that creator's slot goes back, and the survivor then holds a whole
            // pool while its own ceiling reads empty.
            TEST_F(TaskBudget, a_creators_death_does_not_release_a_still_held_object)
            {
                // Task slot order is the fixture's: free_slot() scans upward, so the group
                // that has to DIE and come back must be the lower slot.
                Thread* const doomed = creator_in_task(SLOT_FIRST, 0);
                Task* const doomed_task = doomed->task;
                Thread* const holder = creator_in_task(SLOT_SECOND, 1);

                uint32_t mine[KICKOS_TASK_SEMAPHORE_BUDGET];
                fill_to_the_ceiling(doomed, mine);
                for (int i = 0; i < KICKOS_TASK_SEMAPHORE_BUDGET; i++)
                {
                    ASSERT_NE(delegate(doomed, mine[i], holder), KCAP_INVALID)
                        << "fixture: delegation " << i;
                }

                kernel().current[kickos_kernel_core()] = doomed;
                run_exit(0);
                ASSERT_EQ(task_member_count(doomed_task), 0) << "fixture: the group emptied";
                ASSERT_EQ(live_sems(), KICKOS_TASK_SEMAPHORE_BUDGET)
                    << "fixture: the objects outlived their creator";
                task_drop_hold(doomed_task);

                uint32_t cap = KCAP_INVALID;
                kernel().current[kickos_kernel_core()] = holder;
                EXPECT_EQ(sem_create(0, &cap), -KOS_EOVERFLOW)
                    << "the survivor still holds a ceiling of objects, whoever made them";
                EXPECT_EQ(live_sems(), KICKOS_TASK_SEMAPHORE_BUDGET)
                    << "and the refusal spent no slot";

                // A new task into the slot the dead one held, and it must get its whole
                // ceiling rather than the survivor's leftovers.
                Thread* const fresh = creator_in_task(SLOT_THIRD, 0);
                ASSERT_EQ(fresh->task, doomed_task) << "fixture: the slot came back";
                for (int i = 0; i < KICKOS_TASK_SEMAPHORE_BUDGET; i++)
                {
                    kernel().current[kickos_kernel_core()] = fresh;
                    EXPECT_EQ(sem_create(0, &cap), 0)
                        << "create " << i << " was refused, so the recycled slot inherited a "
                                             "dead task's charge";
                }
            }

            // THE UNIT IS THE POOL SLOT AND NOT THE CAPABILITY. Two names for one object take
            // one slot from the pool, so they cost the holder one. An accounting that counted
            // capabilities would refuse the create below while the pool is untouched.
            TEST_F(TaskBudget, two_capabilities_on_one_object_cost_the_task_one_slot)
            {
                Thread* const maker = creator_in_task(SLOT_FIRST, 0);
                Thread* const holder = creator_in_task(SLOT_SECOND, 1);

                uint32_t cap = KCAP_INVALID;
                kernel().current[kickos_kernel_core()] = maker;
                ASSERT_EQ(sem_create(0, &cap), 0);
                for (int i = 0; i < KICKOS_TASK_SEMAPHORE_BUDGET; i++)
                {
                    ASSERT_NE(delegate(maker, cap, holder), KCAP_INVALID)
                        << "fixture: copy " << i << " of ONE object";
                }
                ASSERT_EQ(live_sems(), 1) << "fixture: the copies are names, not objects";

                uint32_t own = KCAP_INVALID;
                kernel().current[kickos_kernel_core()] = holder;
                EXPECT_EQ(sem_create(0, &own), 0)
                    << "two names for one object leave the holder one slot of headroom";
                kernel().current[kickos_kernel_core()] = holder;
                EXPECT_EQ(sem_create(0, &own), -KOS_EOVERFLOW)
                    << "and the second distinct slot is the ceiling";
            }

            // A CLOSE GIVES THE HOLD BACK, and nothing has to remember to give it. A count
            // moved at the delegation and not at the close would refuse this create.
            TEST_F(TaskBudget, closing_a_delegated_capability_gives_the_hold_back)
            {
                Thread* const maker = creator_in_task(SLOT_FIRST, 0);
                Thread* const holder = creator_in_task(SLOT_SECOND, 1);
                uint32_t mine[KICKOS_TASK_SEMAPHORE_BUDGET];
                fill_to_the_ceiling(maker, mine);

                uint32_t theirs[KICKOS_TASK_SEMAPHORE_BUDGET];
                for (int i = 0; i < KICKOS_TASK_SEMAPHORE_BUDGET; i++)
                {
                    theirs[i] = delegate(maker, mine[i], holder);
                    ASSERT_NE(theirs[i], KCAP_INVALID) << "fixture: delegation " << i;
                }

                uint32_t cap = KCAP_INVALID;
                kernel().current[kickos_kernel_core()] = holder;
                ASSERT_EQ(sem_create(0, &cap), -KOS_EOVERFLOW) << "fixture: at the ceiling";

                ASSERT_EQ(handle_close(holder, theirs[0]), 0);
                ASSERT_EQ(live_sems(), KICKOS_TASK_SEMAPHORE_BUDGET)
                    << "fixture: the maker's own capability keeps the object alive";
                kernel().current[kickos_kernel_core()] = holder;
                EXPECT_EQ(sem_create(0, &cap), 0)
                    << "the closed capability's slot came back to the holder's ceiling";
            }

            // THE BOUND IS THE TASK'S AND NOT THE THREAD'S. A sibling cannot widen it, which
            // is the whole reason the charge is not per capability table.
            TEST_F(TaskBudget, a_siblings_creates_count_against_the_whole_task)
            {
                Thread* const first = creator_in_task(SLOT_FIRST, 0);
                Thread* const sibling = sibling_of(first, SLOT_SECOND);
                ASSERT_EQ(sibling->task, first->task) << "fixture: one group, two threads";

                fill_to_the_ceiling(first, nullptr);
                uint32_t cap = KCAP_INVALID;
                kernel().current[kickos_kernel_core()] = sibling;
                EXPECT_EQ(sem_create(0, &cap), -KOS_EOVERFLOW)
                    << "a thread of a task at its ceiling is at that ceiling too";
                EXPECT_GT(KICKOS_MAX_SEMAPHORES - live_sems(), 0)
                    << "the pool still has slots to spare, so this was not exhaustion";
            }

            // THE SPAWN-SIDE ADMISSION, which is where a delegation is refused rather than
            // counted after the fact. The grant list is admitted as one take against the
            // destination task.
            TEST_F(TaskBudget, a_grant_list_past_the_destinations_ceiling_is_refused)
            {
                Thread* const maker = creator_in_task(SLOT_FIRST, 0);
                Task* const destination = task(1);
                uint32_t mine[KICKOS_TASK_SEMAPHORE_BUDGET];
                fill_to_the_ceiling(maker, mine);

                uint8_t types[KICKOS_TASK_SEMAPHORE_BUDGET + 1];
                int objs[KICKOS_TASK_SEMAPHORE_BUDGET + 1];
                for (int i = 0; i < KICKOS_TASK_SEMAPHORE_BUDGET; i++)
                {
                    types[i] = static_cast<uint8_t>(CapType::CAP_SEM);
                    objs[i] = object_of(maker, mine[i]);
                }
                // One more object than the destination's ceiling admits, taken through the
                // fixture so no task is charged for it.
                int extra = 0;
                (void)semaphore(&extra);

                IrqLock lock;
                EXPECT_TRUE(task_object_admit_grants(destination, types, objs,
                                                     KICKOS_TASK_SEMAPHORE_BUDGET))
                    << "a whole ceiling of distinct objects is admissible into an empty task";

                // The repeats name slots the list already spent, so they cost nothing.
                for (int i = 0; i < KICKOS_TASK_SEMAPHORE_BUDGET; i++)
                {
                    types[KICKOS_TASK_SEMAPHORE_BUDGET] = types[i];
                    objs[KICKOS_TASK_SEMAPHORE_BUDGET] = objs[i];
                    EXPECT_TRUE(task_object_admit_grants(destination, types, objs,
                                                         KICKOS_TASK_SEMAPHORE_BUDGET + 1))
                        << "a repeat of grant " << i << " names a slot the list already took";
                }

                types[KICKOS_TASK_SEMAPHORE_BUDGET] = static_cast<uint8_t>(CapType::CAP_SEM);
                objs[KICKOS_TASK_SEMAPHORE_BUDGET] = extra;
                EXPECT_FALSE(task_object_admit_grants(destination, types, objs,
                                                      KICKOS_TASK_SEMAPHORE_BUDGET + 1))
                    << "one distinct object past the ceiling refuses the whole list";
            }

            // A MIXED GRANT LIST IS MEASURED AGAINST EACH KIND'S OWN CEILING. Every other
            // grant-list arm here is built from CAP_SEM alone, so a spawn-side admission
            // that answered ONE ceiling for the whole list passed all of them: the third
            // kind literal that survived the suite was task_object_admit_grants asking
            // task_object_ceiling(CAP_SEM) instead of task_object_ceiling(type). The two
            // kinds below sit at 2 and 1, so a list can be inside one ceiling and past the
            // other, and which figure decided is readable from the answer.
            TEST_F(TaskBudget, a_mixed_grant_list_is_measured_against_each_kinds_own_ceiling)
            {
                Thread* const maker = creator_in_task(SLOT_FIRST, 0);
                Task* const destination = task(1);
                uint32_t mine[KICKOS_TASK_SEMAPHORE_BUDGET];
                fill_to_the_ceiling(maker, mine);

                // One semaphore and two mutexes past what the list below spends, taken
                // through the fixture so no task is charged for them.
                int spare_sem = 0;
                (void)semaphore(&spare_sem);
                int mtx[KICKOS_TASK_MUTEX_BUDGET + 1];
                for (int i = 0; i < KICKOS_TASK_MUTEX_BUDGET + 1; i++)
                {
                    (void)own_mutex(maker, &mtx[i]);
                }

                constexpr int LIST_MAX =
                    KICKOS_TASK_SEMAPHORE_BUDGET + KICKOS_TASK_MUTEX_BUDGET + 2;
                uint8_t types[LIST_MAX];
                int objs[LIST_MAX];
                int n = 0;
                for (int i = 0; i < KICKOS_TASK_SEMAPHORE_BUDGET; i++)
                {
                    types[n] = static_cast<uint8_t>(CapType::CAP_SEM);
                    objs[n] = object_of(maker, mine[i]);
                    n++;
                }
                for (int i = 0; i < KICKOS_TASK_MUTEX_BUDGET; i++)
                {
                    types[n] = static_cast<uint8_t>(CapType::CAP_MUTEX);
                    objs[n] = mtx[i];
                    n++;
                }
                int const mixed = n;

                IrqLock lock;
                EXPECT_TRUE(task_object_admit_grants(destination, types, objs, mixed))
                    << "a whole ceiling of EACH kind is admissible into an empty task: this "
                       "list is 2 semaphores and 1 mutex, and one figure standing for both "
                       "would refuse the second semaphore at the mutex ceiling of one";

                // One mutex past the MUTEX ceiling while the semaphore half stays inside its
                // own. Only the mutex figure can refuse this.
                types[mixed] = static_cast<uint8_t>(CapType::CAP_MUTEX);
                objs[mixed] = mtx[KICKOS_TASK_MUTEX_BUDGET];
                EXPECT_FALSE(task_object_admit_grants(destination, types, objs, mixed + 1))
                    << "a second mutex is past the mutex ceiling of one, and the semaphore "
                       "budget of two must not be the figure it is measured against";

                // And the other way: one semaphore past the SEMAPHORE ceiling with the mutex
                // half inside its own.
                types[mixed] = static_cast<uint8_t>(CapType::CAP_SEM);
                objs[mixed] = spare_sem;
                EXPECT_FALSE(task_object_admit_grants(destination, types, objs, mixed + 1))
                    << "a third semaphore is past the semaphore ceiling of two, and the "
                       "mutex budget of one must not be the figure it is measured against";
            }

            // A DEAD OBJECT'S SLOT IS STILL A SLOT, AND ITS HOLDER IS WHY. The peer closing
            // does not free this side: an endpoint whose receiver died stays allocated on the
            // client's send cap alone, so the client is charged for it until the client
            // closes. That charge is the mechanism telling a leaking client the truth, and
            // the leak it exposed was real (user/src/newlib_stubs.cc, emit.h, tap.cc all fell
            // back on -KOS_EPIPE and kept the cap).
            TEST_F(TaskBudget, a_dead_endpoints_slot_is_charged_to_the_client_still_holding_it)
            {
                Thread* const server = creator_in_task(SLOT_FIRST, 0);
                Thread* const client = creator_in_task(SLOT_SECOND, 1);

                uint32_t client_caps[KICKOS_TASK_ENDPOINT_BUDGET];
                for (int i = 0; i < KICKOS_TASK_ENDPOINT_BUDGET; i++)
                {
                    Endpoint* const ep = endpoint();
                    int const idx = kernel().endpoints.index_of(ep);
                    int const obj = kernel().endpoints.handle_for(idx);
                    ep->recv_holders = 1;
                    kernel().endpoint_refs[idx] = 1;
                    uint32_t served = KCAP_INVALID;
                    {
                        IrqLock lock;
                        ASSERT_EQ(cap_install(server, obj, CapType::CAP_ENDPOINT,
                                              CAP_WAIT | CAP_SIGNAL, &served), 0);
                        ASSERT_TRUE(obj_ref_inc(CapType::CAP_ENDPOINT, obj, CAP_SIGNAL));
                        ASSERT_EQ(cap_install(client, obj, CapType::CAP_ENDPOINT, CAP_SIGNAL,
                                              &client_caps[i]), 0);
                    }
                    // The driver dies: dropping the last WAIT-bearing cap takes recv_holders
                    // to 0, which is what makes the endpoint answer -KOS_EPIPE.
                    kernel().current[kickos_kernel_core()] = server;
                    ASSERT_EQ(handle_close(server, served), 0);
                    ASSERT_EQ(ep->recv_holders, 0) << "fixture: the endpoint must be DEAD";
                    ASSERT_TRUE(kernel().endpoints.live(idx))
                        << "fixture: the client's cap must still pin the slot";
                }

                IrqLock lock;
                EXPECT_FALSE(task_object_admit(CapType::CAP_ENDPOINT, client->task))
                    << "a client holding a ceiling of DEAD endpoints is at its ceiling: the "
                       "slots are allocated and it is why";
            }

            // AND THE CLOSE GIVES THE SLOT BACK TO THE POOL, not merely to the ceiling. This
            // is what the -KOS_EPIPE arm of the three console clients now does.
            TEST_F(TaskBudget, closing_a_dead_endpoint_returns_its_slot_to_the_pool)
            {
                Thread* const server = creator_in_task(SLOT_FIRST, 0);
                Thread* const client = creator_in_task(SLOT_SECOND, 1);

                Endpoint* const ep = endpoint();
                int const idx = kernel().endpoints.index_of(ep);
                int const obj = kernel().endpoints.handle_for(idx);
                ep->recv_holders = 1;
                kernel().endpoint_refs[idx] = 1;
                uint32_t served = KCAP_INVALID;
                uint32_t held = KCAP_INVALID;
                {
                    IrqLock lock;
                    ASSERT_EQ(cap_install(server, obj, CapType::CAP_ENDPOINT,
                                          CAP_WAIT | CAP_SIGNAL, &served), 0);
                    ASSERT_TRUE(obj_ref_inc(CapType::CAP_ENDPOINT, obj, CAP_SIGNAL));
                    ASSERT_EQ(cap_install(client, obj, CapType::CAP_ENDPOINT, CAP_SIGNAL,
                                          &held), 0);
                }
                kernel().current[kickos_kernel_core()] = server;
                ASSERT_EQ(handle_close(server, served), 0);
                EXPECT_TRUE(kernel().endpoints.live(idx))
                    << "the driver's own close does NOT free the slot: the client pins it";
                EXPECT_EQ(kernel().endpoint_refs[idx], 1);

                kernel().current[kickos_kernel_core()] = client;
                ASSERT_EQ(handle_close(client, held), 0);
                EXPECT_FALSE(kernel().endpoints.live(idx))
                    << "the CLIENT's close is what returns the slot to the pool";
                EXPECT_EQ(kernel().endpoint_refs[idx], 0);
            }

            // A STALE CAPABILITY COSTS ITS HOLDER NOTHING, and the same skip must not spare a
            // LIVE object. The pool is exhausted first so the freed slot is genuinely
            // re-handed rather than bumped past.
            TEST_F(TaskBudget, a_recycled_slot_charges_its_new_holder_and_spares_the_stale_one)
            {
                Thread* const stale = creator_in_task(SLOT_FIRST, 0);
                Thread* const fresh = creator_in_task(SLOT_SECOND, 1);
                int filler_obj = -1;
                for (int f = 1; f < KICKOS_MAX_ENDPOINTS; f++)
                {
                    int const fi = kernel().endpoints.index_of(endpoint());
                    kernel().endpoint_refs[fi] = 1;
                    if (filler_obj < 0)
                    {
                        filler_obj = kernel().endpoints.handle_for(fi);
                    }
                }
                ASSERT_GE(filler_obj, 0) << "fixture: the pool must be wider than one slot";
                Endpoint* const ep = endpoint();
                int const idx = kernel().endpoints.index_of(ep);
                int const obj = kernel().endpoints.handle_for(idx);
                kernel().endpoint_refs[idx] = 1;
                uint32_t old_cap = KCAP_INVALID;
                {
                    IrqLock lock;
                    ASSERT_EQ(cap_install(stale, obj, CapType::CAP_ENDPOINT, CAP_SIGNAL,
                                          &old_cap), 0);
                }
                kernel().endpoints.free(obj);
                kernel().endpoint_refs[idx] = 0;

                Endpoint* const again = kernel().endpoints.at(kernel().endpoints.alloc());
                ASSERT_NE(again, nullptr);
                int const idx2 = kernel().endpoints.index_of(again);
                ASSERT_EQ(idx2, idx) << "fixture: the SAME slot must come back";
                int const obj2 = kernel().endpoints.handle_for(idx2);
                ASSERT_NE(obj2, obj) << "fixture: and under a bumped generation";
                *again = Endpoint{};
                kernel().endpoint_refs[idx2] = 1;
                uint32_t new_cap = KCAP_INVALID;
                {
                    IrqLock lock;
                    ASSERT_EQ(cap_install(fresh, obj2, CapType::CAP_ENDPOINT, CAP_SIGNAL,
                                          &new_cap), 0);
                }

                // ONE LIVE HOLD EACH BESIDE THE TWO UNDER TEST, and the arm is vacuous
                // without it: at a ceiling of two a single miscounted capability still
                // leaves headroom and both checks pass either way. With the filler seated,
                // counting the stale one is the difference between admitted and refused, so
                // dropping SlotPool::live_index's generation compare reddens the first; and
                // NOT counting the live one reddens the second.
                {
                    IrqLock lock;
                    uint32_t filler_cap = KCAP_INVALID;
                    ASSERT_TRUE(obj_ref_inc(CapType::CAP_ENDPOINT, filler_obj, CAP_SIGNAL));
                    ASSERT_EQ(cap_install(stale, filler_obj, CapType::CAP_ENDPOINT, CAP_SIGNAL,
                                          &filler_cap), 0);
                    ASSERT_TRUE(obj_ref_inc(CapType::CAP_ENDPOINT, filler_obj, CAP_SIGNAL));
                    ASSERT_EQ(cap_install(fresh, filler_obj, CapType::CAP_ENDPOINT, CAP_SIGNAL,
                                          &filler_cap), 0);
                }

                IrqLock lock;
                EXPECT_TRUE(task_object_admit(CapType::CAP_ENDPOINT, stale->task))
                    << "a capability naming a freed generation charges its holder nothing";
                EXPECT_FALSE(task_object_admit(CapType::CAP_ENDPOINT, fresh->task))
                    << "and the live occupant is charged to the holder that can use it";
            }

            // WHY THE CLOSE IS KEYED ON -KOS_EPIPE AND NOT ON `r <= 0`. A receiver with no
            // buffer answers ZERO on a LIVE endpoint, so a console fallback that closed on
            // every non-positive return would throw away a working route: the slot survives,
            // the server still holding it, and this client can never reach it again. That is
            // the plausible wrong fix for the three console clients, and this is its cost.
            TEST_F(TaskBudget, closing_a_live_endpoint_loses_the_client_its_route_for_good)
            {
                Thread* const server = creator_in_task(SLOT_FIRST, 0);
                Thread* const client = creator_in_task(SLOT_SECOND, 1);

                Endpoint* const ep = endpoint();
                int const idx = kernel().endpoints.index_of(ep);
                int const obj = kernel().endpoints.handle_for(idx);
                ep->recv_holders = 1;
                kernel().endpoint_refs[idx] = 1;
                uint32_t served = KCAP_INVALID;
                uint32_t held = KCAP_INVALID;
                {
                    IrqLock lock;
                    ASSERT_EQ(cap_install(server, obj, CapType::CAP_ENDPOINT,
                                          CAP_WAIT | CAP_SIGNAL, &served), 0);
                    ASSERT_TRUE(obj_ref_inc(CapType::CAP_ENDPOINT, obj, CAP_SIGNAL));
                    ASSERT_EQ(cap_install(client, obj, CapType::CAP_ENDPOINT, CAP_SIGNAL,
                                          &held), 0);
                }
                kernel().current[kickos_kernel_core()] = client;
                ASSERT_EQ(handle_close(client, held), 0);

                EXPECT_TRUE(kernel().endpoints.live(idx))
                    << "the endpoint is still alive and still served";
                EXPECT_EQ(ep->recv_holders, 1) << "the server never left";
                IrqLock lock;
                EXPECT_EQ(cap_lookup(client, held), nullptr)
                    << "but the client threw its only route away, and nothing gives it back";
            }

            // THE WINDOW A DYING MEMBER OPENS FOR ITS OWN SIBLINGS. sched::exit_current
            // retires the name (Thread::task) BEFORE cap_teardown sweeps the table, and the
            // sweep drops IrqLock every KCAP_TEARDOWN_CHUNK entries. A hold set derived from
            // live members therefore loses the dying member's still-seated capabilities for
            // the whole sweep, while their pool slots are still allocated, and a SIBLING OF
            // THE SAME TASK is admitted against an empty count. Not one slot and not brief: a
            // whole fresh ceiling, reserve included, for the length of a chunked teardown.
            //
            // The survivor must be a SIBLING. tests above put it in another task, where no
            // window exists, which is why none of them can see this.
            namespace dyingwindow
            {
                Thread* g_sibling = nullptr;
                Thread* g_stranger = nullptr;
                int g_admitted = 0;
                int g_last_rc = 0;
                int g_stranger_rc = 0;

                void sibling_creates()
                {
                    while (g_admitted < KICKOS_MAX_SEMAPHORES)
                    {
                        uint32_t cap = KCAP_INVALID;
                        kernel().current[kickos_kernel_core()] = g_sibling;
                        g_last_rc = sem_create(0, &cap);
                        if (g_last_rc != 0)
                        {
                            break;
                        }
                        g_admitted++;
                    }
                    // THE OTHER HALF, and it refuses a predicate keyed on the DYING THREAD
                    // rather than on its task: a thread of ANOTHER group is bounded by its own
                    // ceiling and must not be charged for a stranger's unswept table.
                    if (g_stranger != nullptr)
                    {
                        uint32_t cap = KCAP_INVALID;
                        kernel().current[kickos_kernel_core()] = g_stranger;
                        g_stranger_rc = sem_create(0, &cap);
                    }
                }
            }

            TEST_F(TaskBudget, a_dying_members_holds_still_bound_its_siblings_mid_sweep)
            {
                Thread* const doomed = creator_in_task(SLOT_FIRST, 0);
                Thread* const sibling = sibling_of(doomed, SLOT_SECOND);
                fill_to_the_ceiling(doomed, nullptr);
                ASSERT_EQ(live_sems(), KICKOS_TASK_SEMAPHORE_BUDGET)
                    << "fixture: the group is at its ceiling before the death";

                Thread* const stranger = creator_in_task(SLOT_THIRD, 1);
                dyingwindow::g_sibling = sibling;
                dyingwindow::g_stranger = stranger;
                dyingwindow::g_admitted = 0;
                dyingwindow::g_last_rc = 0;
                dyingwindow::g_stranger_rc = 0;
                // Ordinal 1 is the gap cap_teardown opens BEFORE its first chunk, so the
                // dying member's whole table is still seated when the sibling asks.
                run_in_chunk_gap(dyingwindow::sibling_creates, 1);

                kernel().current[kickos_kernel_core()] = doomed;
                run_exit(0);

                EXPECT_EQ(dyingwindow::g_admitted, 0)
                    << "the sibling was handed " << dyingwindow::g_admitted
                    << " creates while its own group's capabilities were merely unswept";
                EXPECT_EQ(dyingwindow::g_last_rc, -KOS_EOVERFLOW)
                    << "and the refusal must be the budget's own code";
                EXPECT_GT(KICKOS_MAX_SEMAPHORES - live_sems(), 0)
                    << "and the pool was not exhausted by the teardown either";
                EXPECT_EQ(dyingwindow::g_stranger_rc, 0)
                    << "another group's creator must still land: the bound is on the dying "
                       "member's TASK and not on dying threads at large";
            }
        }
    }
}
