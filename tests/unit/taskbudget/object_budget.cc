// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Per-task object budget tests using real task and thread slots.
// Pools and capability tables have spare slots so refusals test the budget.
// Different budgets check that each object kind uses its own limit.
// Endpoint creation is covered by user/apps/common/objbudget.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
#include <kickos/notify.h>
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
            // Leave room for delegated and newly created capabilities so tests reach
            // the object budget before the capability-table limit.
            constexpr uint32_t CAP_WIDTH =
                KICKOS_CAP_FIRST_DYNAMIC + KICKOS_TASK_SEMAPHORE_BUDGET + 4;

            // Use a distinct line per binding, plus one beyond the IRQ budget.
            // These tests do not arm the lines.
            constexpr int LINE_A = 11;
            constexpr int LINE_B = 12;
            constexpr int LINE_C = 13;
            constexpr int LINE_D = 14;
            constexpr int IRQ_LINES[] = {LINE_A, LINE_B, LINE_C, LINE_D};
            static_assert(static_cast<int>(sizeof(IRQ_LINES) / sizeof(IRQ_LINES[0]))
                              > KICKOS_TASK_IRQ_HANDLE_BUDGET,
                          "the IRQ ceiling arm needs a line for every claim inside the "
                          "ceiling and one more for the claim that must be refused");

            // Create a current thread in its own task using a real thread-pool slot.
            Thread* creator_in_task(int slot, int task_slot)
            {
                Task* const group = task(task_slot);
                Thread* const t = seat_pool(slot, PRIO_MID);
                join_task(t, group);
                attach_caps(t, CAP_WIDTH);
                kernel().current[kickos_kernel_core()] = t;
                return t;
            }

            // Join an existing task without allocating another task slot.
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

            int live_notifies()
            {
                int n = 0;
                for (int i = 0; i < KICKOS_MAX_NOTIFY; i++)
                {
                    if (kernel().notifies.live(i))
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

            // Copy a semaphore capability using the same reference and install steps as spawn.
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

            TEST_F(TaskBudget, the_notify_ceiling_refuses_while_the_object_pool_has_slots)
            {
                Thread* const c = creator_in_task(SLOT_FIRST, 0);
                for (int i = 0; i < KICKOS_TASK_NOTIFY_BUDGET; i++)
                {
                    uint32_t cap = KCAP_INVALID;
                    ASSERT_EQ(notify_create(c, &cap), 0)
                        << "create " << i << " is inside the ceiling and must land";
                    ASSERT_NE(cap, KCAP_INVALID);
                }
                ASSERT_EQ(live_notifies(), KICKOS_TASK_NOTIFY_BUDGET);

                uint32_t refused = KCAP_INVALID;
                EXPECT_EQ(notify_create(c, &refused), -KOS_EOVERFLOW)
                    << "past its ceiling the fifth creator is refused, and with the budget's "
                       "own code";
                EXPECT_EQ(refused, KCAP_INVALID) << "a refused create discloses no capability";
                EXPECT_EQ(live_notifies(), KICKOS_TASK_NOTIFY_BUDGET)
                    << "and spends no object slot on the way to the refusal";
                EXPECT_GT(KICKOS_MAX_NOTIFY - live_notifies(), 0)
                    << "the pool still has slots to spare, so this was not exhaustion";
            }

            // A badged capability refers to the same notification and adds no pool usage.
            TEST_F(TaskBudget, a_badged_mint_of_a_held_object_costs_no_budget)
            {
                Thread* const c = creator_in_task(SLOT_FIRST, 0);
                uint32_t first = KCAP_INVALID;
                ASSERT_EQ(notify_create(c, &first), 0);
                for (int i = 1; i < KICKOS_TASK_NOTIFY_BUDGET; i++)
                {
                    uint32_t cap = KCAP_INVALID;
                    ASSERT_EQ(notify_create(c, &cap), 0);
                }
                uint32_t refused = KCAP_INVALID;
                ASSERT_EQ(notify_create(c, &refused), -KOS_EOVERFLOW)
                    << "fixture: the task is at its notification ceiling";

                uint32_t badged = KCAP_INVALID;
                EXPECT_EQ(notify_badge(c, first, 3u, &badged), 0)
                    << "a second name for a slot this task already holds takes no slot";
                EXPECT_NE(badged, KCAP_INVALID);
                EXPECT_EQ(live_notifies(), KICKOS_TASK_NOTIFY_BUDGET)
                    << "and allocated nothing in the pool";
            }

            // A thread binding keeps its notification charged after the capability closes.
            TEST_F(TaskBudget, a_bound_object_stays_charged_after_its_capability_closes)
            {
                Thread* const c = creator_in_task(SLOT_FIRST, 0);
                uint32_t bound = KCAP_INVALID;
                ASSERT_EQ(notify_create(c, &bound), 0);
                ASSERT_EQ(notify_bind(c, bound), 0);
                ASSERT_EQ(handle_close(c, bound), 0);
                ASSERT_EQ(live_notifies(), 1)
                    << "fixture: the bind's own reference keeps the slot allocated";

                for (int i = 1; i < KICKOS_TASK_NOTIFY_BUDGET; i++)
                {
                    uint32_t cap = KCAP_INVALID;
                    ASSERT_EQ(notify_create(c, &cap), 0)
                        << "create " << i << " is inside the ceiling, the bound object being "
                                             "the first of the four";
                }

                uint32_t refused = KCAP_INVALID;
                EXPECT_EQ(notify_create(c, &refused), -KOS_EOVERFLOW)
                    << "the object no capability names is still this task's, and the ceiling "
                       "counts it";
                EXPECT_EQ(refused, KCAP_INVALID) << "a refused create discloses no capability";
                EXPECT_EQ(live_notifies(), KICKOS_TASK_NOTIFY_BUDGET);
                EXPECT_GT(KICKOS_MAX_NOTIFY - live_notifies(), 0)
                    << "the pool still has slots to spare, so this was not exhaustion";
            }

            // Without a capability, the thread releases its binding through the exit path.
            TEST_F(TaskBudget, a_bindings_release_gives_the_hold_back)
            {
                Thread* const c = creator_in_task(SLOT_FIRST, 0);
                uint32_t bound = KCAP_INVALID;
                ASSERT_EQ(notify_create(c, &bound), 0);
                ASSERT_EQ(notify_bind(c, bound), 0);
                ASSERT_EQ(handle_close(c, bound), 0);

                notify_unbind_self(c);
                EXPECT_EQ(live_notifies(), 0)
                    << "the bind held the last reference, so its release frees the slot";

                for (int i = 0; i < KICKOS_TASK_NOTIFY_BUDGET; i++)
                {
                    uint32_t cap = KCAP_INVALID;
                    EXPECT_EQ(notify_create(c, &cap), 0)
                        << "create " << i << " lands on a budget the release gave back";
                }
            }

            // A held IRQ keeps its attached notification charged after the capability closes.
            TEST_F(TaskBudget, an_irq_bindings_object_is_charged_to_the_task_holding_the_line)
            {
                Thread* const c = creator_in_task(SLOT_FIRST, 0);
                uint32_t line = KCAP_INVALID;
                ASSERT_EQ(irq_claim(c, LINE_A, 0, &line), 0);
                uint32_t signalled = KCAP_INVALID;
                ASSERT_EQ(notify_create(c, &signalled), 0);
                ASSERT_EQ(irq_bind_notify(c, line, signalled), 0);
                ASSERT_EQ(handle_close(c, signalled), 0);
                ASSERT_EQ(live_notifies(), 1)
                    << "fixture: the binding's own reference keeps the slot allocated";

                for (int i = 1; i < KICKOS_TASK_NOTIFY_BUDGET; i++)
                {
                    uint32_t cap = KCAP_INVALID;
                    ASSERT_EQ(notify_create(c, &cap), 0)
                        << "create " << i << " is inside the ceiling";
                }

                uint32_t refused = KCAP_INVALID;
                EXPECT_EQ(notify_create(c, &refused), -KOS_EOVERFLOW)
                    << "the object the line raises into is this task's, named by no capability";
                EXPECT_EQ(refused, KCAP_INVALID) << "a refused create discloses no capability";
                EXPECT_EQ(live_notifies(), KICKOS_TASK_NOTIFY_BUDGET);
                EXPECT_GT(KICKOS_MAX_NOTIFY - live_notifies(), 0)
                    << "the pool still has slots to spare, so this was not exhaustion";
            }

            // Capabilities, thread bindings, and IRQ bindings to one notification count once.
            TEST_F(TaskBudget, the_several_ways_to_hold_one_object_cost_one_slot)
            {
                Thread* const c = creator_in_task(SLOT_FIRST, 0);
                uint32_t held = KCAP_INVALID;
                ASSERT_EQ(notify_create(c, &held), 0);
                uint32_t line = KCAP_INVALID;
                ASSERT_EQ(irq_claim(c, LINE_A, 0, &line), 0);
                ASSERT_EQ(irq_bind_notify(c, line, held), 0);
                ASSERT_EQ(notify_bind(c, held), 0);

                for (int i = 1; i < KICKOS_TASK_NOTIFY_BUDGET; i++)
                {
                    uint32_t cap = KCAP_INVALID;
                    EXPECT_EQ(notify_create(c, &cap), 0)
                        << "create " << i << " lands: the three names above are one hold";
                }
                EXPECT_EQ(live_notifies(), KICKOS_TASK_NOTIFY_BUDGET);
            }

            // Grant admission must include the IRQ's attached notification.
            TEST_F(TaskBudget, a_granted_line_stages_the_notification_it_raises_into)
            {
                Thread* const maker = creator_in_task(SLOT_FIRST, 0);
                uint32_t line = KCAP_INVALID;
                ASSERT_EQ(irq_claim(maker, LINE_A, 0, &line), 0);
                uint32_t signalled = KCAP_INVALID;
                ASSERT_EQ(notify_create(maker, &signalled), 0);
                ASSERT_EQ(irq_bind_notify(maker, line, signalled), 0);

                Thread* const dest = creator_in_task(SLOT_SECOND, 1);
                for (int i = 0; i < KICKOS_TASK_NOTIFY_BUDGET; i++)
                {
                    uint32_t cap = KCAP_INVALID;
                    ASSERT_EQ(notify_create(dest, &cap), 0)
                        << "fixture: create " << i << " fills the destination's ceiling";
                }
                ASSERT_GT(KICKOS_MAX_NOTIFY - live_notifies(), 0)
                    << "fixture: the pool still has slots, so a refusal below is the ceiling";

                uint8_t types[1];
                int objs[1];
                types[0] = kcap_grant_pack(static_cast<uint8_t>(CapType::CAP_IRQ), CAP_WAIT);
                objs[0] = object_of(maker, line);

                IrqLock lock;
                EXPECT_FALSE(task_object_admit_grants(dest->task, types, objs, 1))
                    << "the line brings an object the destination has no room for";
            }

            TEST_F(TaskBudget, a_granted_line_inside_the_destinations_ceiling_is_admitted)
            {
                Thread* const maker = creator_in_task(SLOT_FIRST, 0);
                uint32_t line = KCAP_INVALID;
                ASSERT_EQ(irq_claim(maker, LINE_A, 0, &line), 0);
                uint32_t signalled = KCAP_INVALID;
                ASSERT_EQ(notify_create(maker, &signalled), 0);
                ASSERT_EQ(irq_bind_notify(maker, line, signalled), 0);

                Thread* const dest = creator_in_task(SLOT_SECOND, 1);
                for (int i = 0; i < KICKOS_TASK_NOTIFY_BUDGET - 1; i++)
                {
                    uint32_t cap = KCAP_INVALID;
                    ASSERT_EQ(notify_create(dest, &cap), 0);
                }

                uint8_t types[1];
                int objs[1];
                types[0] = kcap_grant_pack(static_cast<uint8_t>(CapType::CAP_IRQ), CAP_WAIT);
                objs[0] = object_of(maker, line);

                IrqLock lock;
                EXPECT_TRUE(task_object_admit_grants(dest->task, types, objs, 1))
                    << "one slot of room is what the line's object needs";
            }

            TEST_F(TaskBudget, a_granted_line_naming_a_held_object_costs_the_destination_nothing)
            {
                Thread* const maker = creator_in_task(SLOT_FIRST, 0);
                uint32_t line = KCAP_INVALID;
                ASSERT_EQ(irq_claim(maker, LINE_A, 0, &line), 0);
                uint32_t signalled = KCAP_INVALID;
                ASSERT_EQ(notify_create(maker, &signalled), 0);
                ASSERT_EQ(irq_bind_notify(maker, line, signalled), 0);
                int const shared = object_of(maker, signalled);

                Thread* const dest = creator_in_task(SLOT_SECOND, 1);
                for (int i = 0; i < KICKOS_TASK_NOTIFY_BUDGET - 1; i++)
                {
                    uint32_t cap = KCAP_INVALID;
                    ASSERT_EQ(notify_create(dest, &cap), 0);
                }
                {
                    IrqLock lock;
                    uint32_t theirs = KCAP_INVALID;
                    ASSERT_TRUE(obj_ref_inc(CapType::CAP_NOTIFY, shared, CAP_SIGNAL));
                    ASSERT_EQ(cap_install(dest, shared, CapType::CAP_NOTIFY, CAP_SIGNAL, &theirs),
                              0);
                }

                uint8_t types[1];
                int objs[1];
                types[0] = kcap_grant_pack(static_cast<uint8_t>(CapType::CAP_IRQ), CAP_WAIT);
                objs[0] = object_of(maker, line);

                IrqLock lock;
                EXPECT_TRUE(task_object_admit_grants(dest->task, types, objs, 1))
                    << "the destination is at its ceiling and the line brings it no new slot";
            }

            // Check the recipient's budget when attachment happens after delegation.
            TEST_F(TaskBudget, an_attach_is_refused_where_a_peer_holding_the_line_has_no_room)
            {
                Thread* const donor = creator_in_task(SLOT_FIRST, 0);
                uint32_t line = KCAP_INVALID;
                ASSERT_EQ(irq_claim(donor, LINE_A, 0, &line), 0);
                int const binding = object_of(donor, line);

                Thread* const peer = creator_in_task(SLOT_SECOND, 1);
                {
                    IrqLock lock;
                    uint32_t theirs = KCAP_INVALID;
                    ASSERT_TRUE(obj_ref_inc(CapType::CAP_IRQ, binding, CAP_WAIT));
                    ASSERT_EQ(cap_install(peer, binding, CapType::CAP_IRQ, CAP_WAIT, &theirs), 0);
                }
                for (int i = 0; i < KICKOS_TASK_NOTIFY_BUDGET; i++)
                {
                    uint32_t cap = KCAP_INVALID;
                    ASSERT_EQ(notify_create(peer, &cap), 0)
                        << "fixture: create " << i << " fills the peer's ceiling";
                }

                uint32_t signalled = KCAP_INVALID;
                kernel().current[kickos_kernel_core()] = donor;
                ASSERT_EQ(notify_create(donor, &signalled), 0);
                EXPECT_EQ(irq_bind_notify(donor, line, signalled), -KOS_EOVERFLOW)
                    << "the attach would seat this object on a peer that has no room for it";
                EXPECT_GT(KICKOS_MAX_NOTIFY - live_notifies(), 0)
                    << "the pool still has slots to spare, so this was not exhaustion";
            }

            TEST_F(TaskBudget, an_attach_lands_where_every_holder_of_the_line_has_room)
            {
                Thread* const donor = creator_in_task(SLOT_FIRST, 0);
                uint32_t line = KCAP_INVALID;
                ASSERT_EQ(irq_claim(donor, LINE_A, 0, &line), 0);
                int const binding = object_of(donor, line);

                Thread* const peer = creator_in_task(SLOT_SECOND, 1);
                {
                    IrqLock lock;
                    uint32_t theirs = KCAP_INVALID;
                    ASSERT_TRUE(obj_ref_inc(CapType::CAP_IRQ, binding, CAP_WAIT));
                    ASSERT_EQ(cap_install(peer, binding, CapType::CAP_IRQ, CAP_WAIT, &theirs), 0);
                }
                for (int i = 0; i < KICKOS_TASK_NOTIFY_BUDGET - 1; i++)
                {
                    uint32_t cap = KCAP_INVALID;
                    ASSERT_EQ(notify_create(peer, &cap), 0);
                }

                uint32_t signalled = KCAP_INVALID;
                kernel().current[kickos_kernel_core()] = donor;
                ASSERT_EQ(notify_create(donor, &signalled), 0);
                EXPECT_EQ(irq_bind_notify(donor, line, signalled), 0)
                    << "one slot of room in the peer is what this attach needs";
            }

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
                kernel().current[kickos_kernel_core()] = second;
                EXPECT_EQ(sem_create(0, &cap), 0) << "the ceiling is per task, not shared";
                kernel().current[kickos_kernel_core()] = second;
                EXPECT_EQ(sem_create(0, &cap), -KOS_EOVERFLOW)
                    << "and the second task is bounded by the same ceiling";
            }

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

            // IRQ release detaches the line; SMP also defers slot reuse until reclamation.
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

            // Delegation copies a reference, so both tasks count the object.
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

            // The surviving holder must still count the object after the creator exits.
            TEST_F(TaskBudget, a_creators_death_does_not_release_a_still_held_object)
            {
                // free_slot() scans upward; use the lower slot for the task that must be reused.
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

            TEST_F(TaskBudget, a_grant_list_past_the_destinations_ceiling_is_refused)
            {
                Thread* const maker = creator_in_task(SLOT_FIRST, 0);
                Task* const destination = task(1);
                uint32_t mine[KICKOS_TASK_SEMAPHORE_BUDGET];
                fill_to_the_ceiling(maker, mine);

                // Pack kinds and rights as the spawn grant list does.
                uint8_t types[KICKOS_TASK_SEMAPHORE_BUDGET + 1];
                int objs[KICKOS_TASK_SEMAPHORE_BUDGET + 1];
                for (int i = 0; i < KICKOS_TASK_SEMAPHORE_BUDGET; i++)
                {
                    types[i] = kcap_grant_pack(static_cast<uint8_t>(CapType::CAP_SEM), CAP_WAIT);
                    objs[i] = object_of(maker, mine[i]);
                }
                // Allocate through the fixture without charging a task.
                int extra = 0;
                (void)semaphore(&extra);

                IrqLock lock;
                EXPECT_TRUE(task_object_admit_grants(destination, types, objs,
                                                     KICKOS_TASK_SEMAPHORE_BUDGET))
                    << "a whole ceiling of distinct objects is admissible into an empty task";

                for (int i = 0; i < KICKOS_TASK_SEMAPHORE_BUDGET; i++)
                {
                    types[KICKOS_TASK_SEMAPHORE_BUDGET] = types[i];
                    objs[KICKOS_TASK_SEMAPHORE_BUDGET] = objs[i];
                    EXPECT_TRUE(task_object_admit_grants(destination, types, objs,
                                                         KICKOS_TASK_SEMAPHORE_BUDGET + 1))
                        << "a repeat of grant " << i << " names a slot the list already took";
                }

                types[KICKOS_TASK_SEMAPHORE_BUDGET] =
                    kcap_grant_pack(static_cast<uint8_t>(CapType::CAP_SEM), CAP_WAIT);
                objs[KICKOS_TASK_SEMAPHORE_BUDGET] = extra;
                EXPECT_FALSE(task_object_admit_grants(destination, types, objs,
                                                      KICKOS_TASK_SEMAPHORE_BUDGET + 1))
                    << "one distinct object past the ceiling refuses the whole list";
            }

            // Use different semaphore and mutex budgets to check each limit independently.
            TEST_F(TaskBudget, a_mixed_grant_list_is_measured_against_each_kinds_own_ceiling)
            {
                Thread* const maker = creator_in_task(SLOT_FIRST, 0);
                Task* const destination = task(1);
                uint32_t mine[KICKOS_TASK_SEMAPHORE_BUDGET];
                fill_to_the_ceiling(maker, mine);

                // Allocate extra objects through the fixture without charging a task.
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
                    types[n] = kcap_grant_pack(static_cast<uint8_t>(CapType::CAP_SEM), CAP_WAIT);
                    objs[n] = object_of(maker, mine[i]);
                    n++;
                }
                for (int i = 0; i < KICKOS_TASK_MUTEX_BUDGET; i++)
                {
                    types[n] = kcap_grant_pack(static_cast<uint8_t>(CapType::CAP_MUTEX), CAP_WAIT);
                    objs[n] = mtx[i];
                    n++;
                }
                int const mixed = n;

                IrqLock lock;
                EXPECT_TRUE(task_object_admit_grants(destination, types, objs, mixed))
                    << "a whole ceiling of EACH kind is admissible into an empty task: this "
                       "list is 2 semaphores and 1 mutex, and one figure standing for both "
                       "would refuse the second semaphore at the mutex ceiling of one";

                // Exceed only the mutex budget.
                types[mixed] = kcap_grant_pack(static_cast<uint8_t>(CapType::CAP_MUTEX), CAP_WAIT);
                objs[mixed] = mtx[KICKOS_TASK_MUTEX_BUDGET];
                EXPECT_FALSE(task_object_admit_grants(destination, types, objs, mixed + 1))
                    << "a second mutex is past the mutex ceiling of one, and the semaphore "
                       "budget of two must not be the figure it is measured against";

                // Exceed only the semaphore budget.
                types[mixed] = kcap_grant_pack(static_cast<uint8_t>(CapType::CAP_SEM), CAP_WAIT);
                objs[mixed] = spare_sem;
                EXPECT_FALSE(task_object_admit_grants(destination, types, objs, mixed + 1))
                    << "a third semaphore is past the semaphore ceiling of two, and the "
                       "mutex budget of one must not be the figure it is measured against";
            }

            // An endpoint stays allocated and charged while the client holds a send capability,
            // even after its receiver exits.
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
                    // Dropping the last WAIT capability makes the endpoint return -KOS_EPIPE.
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

            // Exhaust the pool to force reuse of the freed slot and test its generation check.
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

                // Add one valid hold per task. With a budget of two, either counting the stale
                // capability or ignoring the live one changes the admission result.
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

            // A receiver with no buffer returns zero on a live endpoint.
            // Only -KOS_EPIPE should trigger the console client's close path.
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

            // Keep the exiting thread's capabilities charged to its task until teardown ends.
            // cap_teardown releases IrqLock between chunks, allowing siblings to allocate.
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
                    // An exiting thread's capabilities must not count against another task.
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
                // Ordinal 1 is the gap before the first teardown chunk; all capabilities remain.
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
