// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Check task-pool exhaustion, error values and cleanup while other pools
// still have capacity. These tests call task_for/task_create directly;
// they do not exercise the syscall return path.

#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

#include "kseam_test.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class TaskPool : public KSeam
            {
            };

            int live_domains()
            {
                int n = 0;
                for (int i = 0; i < FIXTURE_DOMAIN_SLOTS; i++)
                {
                    if (g_domain_live[i])
                    {
                        n++;
                    }
                }
                return n;
            }

            int free_task_slots()
            {
                Kernel& k = kernel();
                int n = 0;
                for (int i = 0; i < KICKOS_MAX_TASKS; i++)
                {
                    if (k.tasks[i].refcount == 0
                        and k.tasks[i].creator_tag == ThreadPool::KILL_TAG_NONE)
                    {
                        n++;
                    }
                }
                return n;
            }

            // Reserve every slot with a creator hold and no live threads.
            void fill_task_pool()
            {
                for (int i = 0; i < KICKOS_MAX_TASKS; i++)
                {
                    task(i);
                }
            }

            Task* spawn_task(int* err)
            {
                *err = 0;
                return task_for(DOM_CALLER_MEM_AUTH, nullptr, 0, nullptr, err);
            }
        }

        TEST_F(TaskPool, a_full_pool_refuses_a_spawn_with_enomem)
        {
            fill_task_pool();
            ASSERT_EQ(free_task_slots(), 0);

            int err = 0;
            EXPECT_EQ(spawn_task(&err), nullptr);
            EXPECT_EQ(err, KOS_ENOMEM);
        }

        // Task admission fails before thread-slot allocation.
        TEST_F(TaskPool, a_refused_spawn_leaves_the_thread_pool_alone)
        {
            fill_task_pool();
            int const seated_before = kernel().threads.next;

            int err = 0;
            ASSERT_EQ(spawn_task(&err), nullptr);

            EXPECT_EQ(kernel().threads.next, seated_before);
            int const slot = kernel().threads.alloc();
            EXPECT_GE(slot, 0);
        }

        // A refused task must release its newly admitted domain.
        TEST_F(TaskPool, a_refused_spawn_gives_back_the_domain_it_admitted)
        {
            fill_task_pool();
            int const held = live_domains();
            ASSERT_EQ(held, KICKOS_MAX_TASKS);

            int err = 0;
            ASSERT_EQ(spawn_task(&err), nullptr);

            EXPECT_EQ(live_domains(), held);
        }

        TEST_F(TaskPool, a_full_pool_refuses_an_explicit_task_and_gives_its_domain_back)
        {
            fill_task_pool();
            int const held = live_domains();
            int const holds = kernel().task_holds;

            int err = 0;
            Task* const tk = task_create(FIXTURE_TASK_TAG, /*caller=*/0u, nullptr, 0,
                                         /*mem_attr=*/0u, /*donor=*/nullptr, &err);

            EXPECT_EQ(tk, nullptr);
            EXPECT_EQ(err, KOS_ENOMEM);
            EXPECT_EQ(live_domains(), held);
            EXPECT_EQ(kernel().task_holds, holds);
        }

        // A released slot must be reusable. task_for leaves it unreferenced until task_ref.
        TEST_F(TaskPool, a_released_slot_makes_the_next_spawn_succeed)
        {
            fill_task_pool();
            int err = 0;
            ASSERT_EQ(spawn_task(&err), nullptr);

            task_drop_hold(&kernel().tasks[KICKOS_MAX_TASKS - 1]);
            ASSERT_EQ(free_task_slots(), 1);

            Task* const tk = spawn_task(&err);
            ASSERT_NE(tk, nullptr);
            EXPECT_EQ(err, 0);
            task_ref(tk);
            EXPECT_EQ(free_task_slots(), 0);
        }
    }
}
