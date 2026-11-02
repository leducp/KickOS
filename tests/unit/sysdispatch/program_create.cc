// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kos_program_find and kos_task_create over image 0's declarations: the instance a create starts,
// what it refuses before anything is spent, and the entry thread it starts with its argv.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/image.h>

#include <stddef.h>
#include <string.h>

#include <type_traits>

#include "kseam_test.h"
#include "../common/seam_pools.h"
#include "region_seam.h"
#include "syscall_internal.h"

extern "C" uint64_t syscall_dispatch(uintptr_t nr, uintptr_t a0, uintptr_t a1, uintptr_t a2,
                                     uintptr_t a3);

namespace
{
    template <typename T, typename = void>
    struct HasTask : std::false_type
    {
    };
    template <typename T>
    struct HasTask<T, std::void_t<decltype(&T::task)>> : std::true_type
    {
    };
    template <typename T, typename = void>
    struct HasMemBase : std::false_type
    {
    };
    template <typename T>
    struct HasMemBase<T, std::void_t<decltype(&T::mem_base)>> : std::true_type
    {
    };
    static_assert(not HasTask<kos_thread_params>::value,
                  "a thread joins its creator's task and names no other");
    static_assert(not HasMemBase<kos_thread_params>::value,
                  "a thread brings no data block of its own");

    constexpr uint8_t PRIO_CREATOR = 8;
    constexpr uint8_t PRIO_PEER = 6;

    // The declarations and their names, one object as the composer emits it.
    struct Decls
    {
        kos_task_decl decl[4];
        char names[24];
    };

    constexpr uint32_t name_at(uint32_t at)
    {
        return static_cast<uint32_t>(offsetof(Decls, names)) + at;
    }

    // main as the init starts it; peer0 on request, holding nothing; peer1 on request, holding
    // the line authority; back as the init starts it, declared `restart`.
    Decls const DECLS = {
        {
            {name_at(0), 0u, 0u, 0u, 1024u, 0u, 0u, PRIO_PEER, PRIO_PEER, 0u, 0u, KICKOS_SEAM_DECL_BUDGETS},
            {name_at(5), 0u, 0u, 0u, 1024u, 0u, KOS_DECL_ON_REQUEST, PRIO_PEER, PRIO_PEER, 0u, 0u, KICKOS_SEAM_DECL_BUDGETS},
            {name_at(11), 0u, 0u, KOS_AUTH_IRQ, 1024u, 0u, KOS_DECL_ON_REQUEST, PRIO_PEER, PRIO_PEER, 0u, 0u, KICKOS_SEAM_DECL_BUDGETS},
            {name_at(17), 0u, 0u, 0u, 1024u, 0u, KOS_DECL_RESTARTS, PRIO_PEER, PRIO_PEER, 0u, 0u, KICKOS_SEAM_DECL_BUDGETS},
        },
        "main\0peer0\0peer1\0back",
    };

    void decl_entry(void*) {}

    // What a create names, in a region its caller holds.
    struct alignas(kickos::testfix::REGION_MIN) Block
    {
        kos_task_params p;
        kos_thread_params spawn;
        char const* argv[KICKOS_MAX_ARGC + 2];
        char strings[64];
    };
    Block g_block;

    char const* text(char const* s, size_t at)
    {
        strcpy(&g_block.strings[at], s);
        return &g_block.strings[at];
    }
}

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            class ProgramCreate : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    g_arena_used = 0;
                    declare(DECLS.decl, 4);
                    program("app", reinterpret_cast<uintptr_t>(&decl_entry));
                    g_block = {};
                }

                // A creator holding `authority`, its table holding the program at `prog_`, a
                // member of a task whose grant tops out at `ceiling` where it is not 0, in pool
                // slot `slot`, root's at 0.
                void creator(uint32_t authority, uint8_t ceiling = 0, int slot = 2)
                {
                    Kernel& k = kernel();
                    Task* const own = task(0);
                    if (ceiling != 0)
                    {
                        ASSERT_EQ(task_sched_narrow(own, ceiling, 0u), 0);
                    }
                    Thread* const c = seat_pool(slot, PRIO_CREATOR);
                    join_task(c, own);
                    attach_caps(c, KICKOS_SEAM_CAP_WIDTH);
                    cap_seat_authority(c, authority);
                    k.current(kickos_kernel_core()) = c;
                    ASSERT_TRUE(c->mpu.add(reinterpret_cast<uintptr_t>(&g_block), sizeof(g_block),
                                           ARCH_MPU_R | ARCH_MPU_W));
                    ASSERT_EQ(program_find_call(reinterpret_cast<uintptr_t>(text("app", 32)), &prog_),
                              0);
                }

                int create(char const* instance, kos_task_t* out)
                {
                    g_block.p.instance = nullptr;
                    if (instance != nullptr)
                    {
                        g_block.p.instance = text(instance, 48);
                    }
                    return task_create_call(prog_, reinterpret_cast<uintptr_t>(&g_block.p), out);
                }

                // The entry thread of task `h`.
                Thread* entry_of(kos_task_t h)
                {
                    IrqLock lock;
                    Task* const t = task_resolve(h);
                    for (Thread& th : kernel().threads.all())
                    {
                        if (t != nullptr and th.task == t and th.task_role == kickos::TASK_ROLE_ENTRY)
                        {
                            return &th;
                        }
                    }
                    return nullptr;
                }

                // Runs task `h`'s entry thread to its exit, the creator current again after.
                void end(kos_task_t h)
                {
                    Thread* const c = sched::current();
                    Thread* const e = entry_of(h);
                    ASSERT_NE(e, nullptr);
                    kernel().current(kickos_kernel_core()) = e;
                    run_exit(0);
                    kernel().current(kickos_kernel_core()) = c;
                }

                uint32_t prog_ = KCAP_INVALID;
            };
        }

        TEST_F(ProgramCreate, a_program_is_found_by_its_name_and_no_other)
        {
            creator(AUTH_TASKS);
            int err = 0;
            IrqLock lock;
            EXPECT_NE(cap_resolve_e(sched::current(), prog_, CapType::CAP_PROGRAM, 0, &err), nullptr);
        }

        TEST_F(ProgramCreate, an_unknown_program_is_no_entry)
        {
            creator(AUTH_TASKS);
            uint32_t prog = KCAP_INVALID;
            EXPECT_EQ(program_find_call(reinterpret_cast<uintptr_t>(text("ap", 40)), &prog), -KOS_ENOENT);
            EXPECT_EQ(prog, KOS_CAP_NONE);
        }

        TEST_F(ProgramCreate, a_name_the_caller_cannot_read_is_a_fault)
        {
            creator(AUTH_TASKS);
            static char const unheld[] = "app";
            uint32_t prog = KCAP_INVALID;
            EXPECT_EQ(program_find_call(reinterpret_cast<uintptr_t>(unheld), &prog), -KOS_EFAULT);
        }

        TEST_F(ProgramCreate, the_named_instance_starts_at_its_declared_priority_with_its_name_as_argv)
        {
            creator(AUTH_TASKS);
            kos_task_t h = KOS_TASK_NONE;
            ASSERT_EQ(create("peer0", &h), 0);
            Thread* const e = entry_of(h);
            ASSERT_NE(e, nullptr);
            EXPECT_EQ(e->base_prio, PRIO_PEER);
            EXPECT_STREQ(e->name, "peer0");
            auto const top = static_cast<unsigned char*>(e->stack_base) + e->stack_size;
            auto const a = reinterpret_cast<uintptr_t const*>(top - 48u);
            EXPECT_EQ(a[0], 1u) << "argc";
            EXPECT_EQ(a[1], 1u) << "the declaration's index";
            EXPECT_EQ(a[2], 0u) << "the shared block, none named";
            EXPECT_STREQ(reinterpret_cast<char const*>(a[3]), "peer0");
            EXPECT_EQ(a[4], 0u);
        }

        TEST_F(ProgramCreate, the_argv_a_creator_passes_is_copied_to_the_entry_stack)
        {
            creator(AUTH_TASKS);
            g_block.argv[0] = text("peer0", 0);
            g_block.argv[1] = text("role", 8);
            g_block.argv[2] = text("x", 16);
            g_block.argv[3] = nullptr;
            g_block.p.argv = g_block.argv;
            kos_task_t h = KOS_TASK_NONE;
            ASSERT_EQ(create("peer0", &h), 0);
            Thread* const e = entry_of(h);
            ASSERT_NE(e, nullptr);
            auto const top = static_cast<unsigned char*>(e->stack_base) + e->stack_size;
            // Three pointers and a NULL past the three words, then 6 + 5 + 2 bytes, rounded to 16.
            size_t const bytes = (7u * sizeof(uintptr_t) + 13u + 15u) & ~size_t{15u};
            auto const a = reinterpret_cast<uintptr_t const*>(top - bytes);
            ASSERT_EQ(a[0], 3u);
            EXPECT_STREQ(reinterpret_cast<char const*>(a[3]), "peer0");
            EXPECT_STREQ(reinterpret_cast<char const*>(a[4]), "role");
            EXPECT_STREQ(reinterpret_cast<char const*>(a[5]), "x");
            EXPECT_EQ(a[6], 0u);
            EXPECT_GE(reinterpret_cast<uintptr_t>(a[3]), reinterpret_cast<uintptr_t>(e->stack_base))
                << "the strings are the kernel's copy on the child's stack";
        }

        TEST_F(ProgramCreate, a_reserved_byte_not_zero_is_refused)
        {
            creator(AUTH_TASKS);
            uint32_t const holds = kernel().task_holds;
            g_block.p.rsv0 = 1u;
            kos_task_t h = KOS_TASK_NONE;
            EXPECT_EQ(create("peer0", &h), -KOS_EINVAL);
            EXPECT_EQ(kernel().task_holds, holds) << "a refused create spent a task";
            g_block.p.rsv0 = 0u;
            EXPECT_EQ(create("peer0", &h), 0);
        }

        TEST_F(ProgramCreate, an_argv_of_more_strings_than_the_bound_is_refused)
        {
            creator(AUTH_TASKS);
            char const* const empty = text("", 0);
            for (size_t i = 0; i <= KICKOS_MAX_ARGC; i++)
            {
                g_block.argv[i] = empty;
            }
            g_block.argv[KICKOS_MAX_ARGC + 1] = nullptr;
            g_block.p.argv = g_block.argv;
            kos_task_t h = KOS_TASK_NONE;
            EXPECT_EQ(create("peer0", &h), -KOS_EINVAL);
            g_block.argv[KICKOS_MAX_ARGC] = nullptr;
            ASSERT_EQ(create("peer0", &h), 0) << "the bound itself is taken";
            Thread* const e = entry_of(h);
            ASSERT_NE(e, nullptr);
            auto const top = static_cast<unsigned char*>(e->stack_base) + e->stack_size;
            size_t const bytes = ((4u + KICKOS_MAX_ARGC) * sizeof(uintptr_t) + KICKOS_MAX_ARGC + 15u)
                                 & ~size_t{15u};
            auto const a = reinterpret_cast<uintptr_t const*>(top - bytes);
            EXPECT_EQ(a[0], static_cast<uintptr_t>(KICKOS_MAX_ARGC));
        }

        TEST_F(ProgramCreate, an_instance_takes_its_declared_ceiling)
        {
            creator(AUTH_TASKS);
            kos_task_t h = KOS_TASK_NONE;
            ASSERT_EQ(create("peer0", &h), 0);
            IrqLock lock;
            Task* const t = task_resolve(h);
            ASSERT_NE(t, nullptr);
            EXPECT_EQ(task_prio_ceiling(t), PRIO_PEER);
        }

        TEST_F(ProgramCreate, a_declared_ceiling_above_the_creator_s_own_is_refused)
        {
            creator(AUTH_TASKS, static_cast<uint8_t>(PRIO_PEER - 1u));
            uint32_t const holds = kernel().task_holds;
            kos_task_t h = KOS_TASK_NONE;
            EXPECT_EQ(create("peer0", &h), -KOS_EPERM);
            EXPECT_EQ(h, KOS_TASK_NONE);
            EXPECT_EQ(kernel().task_holds, holds) << "a refused create spent a task";
        }

        TEST_F(ProgramCreate, a_live_named_instance_is_busy)
        {
            creator(AUTH_TASKS | AUTH_IRQ);
            kos_task_t h = KOS_TASK_NONE;
            ASSERT_EQ(create("peer0", &h), 0);
            uint32_t const holds = kernel().task_holds;
            kos_task_t again = KOS_TASK_NONE;
            EXPECT_EQ(create("peer0", &again), -KOS_EBUSY);
            EXPECT_EQ(again, KOS_TASK_NONE);
            EXPECT_EQ(kernel().task_holds, holds) << "a refused create spent a task";
        }

        TEST_F(ProgramCreate, no_instance_named_starts_the_first_free_one_then_none_is_left)
        {
            creator(AUTH_TASKS | AUTH_IRQ);
            kos_task_t first = KOS_TASK_NONE;
            kos_task_t second = KOS_TASK_NONE;
            ASSERT_EQ(create(nullptr, &first), 0);
            ASSERT_EQ(create(nullptr, &second), 0);
            EXPECT_STREQ(entry_of(first)->name, "peer0");
            EXPECT_STREQ(entry_of(second)->name, "peer1");
            kos_task_t third = KOS_TASK_NONE;
            EXPECT_EQ(create(nullptr, &third), -KOS_ENOMEM) << "every instance it may start is live";
        }

        TEST_F(ProgramCreate, a_declared_authority_the_creator_lacks_is_refused)
        {
            creator(AUTH_TASKS);
            uint32_t const holds = kernel().task_holds;
            kos_task_t h = KOS_TASK_NONE;
            EXPECT_EQ(create("peer1", &h), -KOS_EPERM);
            EXPECT_EQ(kernel().task_holds, holds) << "a refused create spent a task";
            ASSERT_EQ(create(nullptr, &h), 0);
            EXPECT_EQ(create(nullptr, &h), -KOS_ENOMEM) << "the one it lacks the authority for is skipped";
        }

        // With no instance named and none startable, a refusal that tells never and retry from
        // every instance live.
        TEST_F(ProgramCreate, no_instance_named_answers_never_where_the_caller_may_start_none)
        {
            creator(AUTH_TASKS | AUTH_IRQ, static_cast<uint8_t>(PRIO_PEER - 1u));
            uint32_t const holds = kernel().task_holds;
            kos_task_t h = KOS_TASK_NONE;
            EXPECT_EQ(create(nullptr, &h), -KOS_EPERM) << "every ceiling is above the creator's";
            EXPECT_EQ(kernel().task_holds, holds) << "a refused create spent a task";
        }

        TEST_F(ProgramCreate, no_instance_named_answers_retry_where_one_is_still_dying)
        {
            creator(AUTH_TASKS | AUTH_IRQ);
            Thread* const c = sched::current();
            kos_task_t first = KOS_TASK_NONE;
            kos_task_t second = KOS_TASK_NONE;
            ASSERT_EQ(create(nullptr, &first), 0);
            ASSERT_EQ(create(nullptr, &second), 0);
            Thread* const e = entry_of(first);
            ASSERT_NE(e, nullptr);
            ASSERT_EQ(task_slay(first, 0), 0);
            kos_task_t h = KOS_TASK_NONE;
            EXPECT_EQ(create(nullptr, &h), -KOS_EAGAIN)
                << "one instance's members still die while the other is live";
            kernel().current(kickos_kernel_core()) = e;
            run_exit(0);
            kernel().current(kickos_kernel_core()) = c;
            EXPECT_EQ(create(nullptr, &h), 0);
            EXPECT_STREQ(entry_of(h)->name, "peer0");
        }

        TEST_F(ProgramCreate, an_instance_the_init_starts_is_refused_to_any_other_creator)
        {
            creator(AUTH_TASKS | AUTH_IRQ);
            kos_task_t h = KOS_TASK_NONE;
            EXPECT_EQ(create("main", &h), -KOS_EPERM);
        }

        TEST_F(ProgramCreate, an_instance_the_program_does_not_declare_is_no_entry)
        {
            creator(AUTH_TASKS);
            kos_task_t h = KOS_TASK_NONE;
            EXPECT_EQ(create("peer", &h), -KOS_ENOENT);
        }

        TEST_F(ProgramCreate, a_creator_without_the_task_authority_is_refused)
        {
            creator(AUTH_IRQ);
            kos_task_t h = KOS_TASK_NONE;
            EXPECT_EQ(create("peer0", &h), -KOS_EPERM);
        }

        TEST_F(ProgramCreate, an_instance_is_free_again_once_it_is_dead)
        {
            creator(AUTH_TASKS);
            Thread* const c = sched::current();
            kos_task_t h = KOS_TASK_NONE;
            ASSERT_EQ(create("peer0", &h), 0);
            Thread* const e = entry_of(h);
            ASSERT_NE(e, nullptr);
            kernel().current(kickos_kernel_core()) = e;
            run_exit(0);
            kernel().current(kickos_kernel_core()) = c;
            {
                IrqLock lock;
                EXPECT_EQ(task_decl_state(1), 0) << "a dead instance still claims its declaration";
            }
            kos_task_t next = KOS_TASK_NONE;
            EXPECT_EQ(create("peer0", &next), 0);
        }

        TEST_F(ProgramCreate, the_next_start_reuses_the_instance_s_record_and_ends_the_kept_handle)
        {
            creator(AUTH_TASKS);
            Thread* const c = sched::current();
            uint32_t const holds = kernel().task_holds;
            kos_task_t kept = KOS_TASK_NONE;
            for (int i = 0; i < 1000; i++)
            {
                kos_task_t h = KOS_TASK_NONE;
                ASSERT_EQ(create("peer0", &h), 0) << "start " << i;
                if (kept != KOS_TASK_NONE)
                {
                    EXPECT_EQ(handle_index(h), handle_index(kept)) << "a record per incarnation";
                    int status = 0;
                    EXPECT_EQ(task_state_call(kept, &status), -KOS_EBADF)
                        << "the handle of the dead incarnation still names a task";
                }
                Thread* const e = entry_of(h);
                ASSERT_NE(e, nullptr);
                kernel().current(kickos_kernel_core()) = e;
                run_exit(0);
                kernel().current(kickos_kernel_core()) = c;
                kept = h;
            }
            EXPECT_EQ(kernel().task_holds, holds + 1u) << "only the last incarnation is held";
        }

        TEST_F(ProgramCreate, a_kept_handle_s_next_start_takes_the_dead_one_s_domain_on_a_full_pool)
        {
            creator(AUTH_TASKS);
            Thread* const c = sched::current();
            for (int i = 0; i < 3; i++)
            {
                kos_task_t h = KOS_TASK_NONE;
                ASSERT_EQ(create("peer0", &h), 0) << "start " << i << " on a full domain pool";
                Thread* const e = entry_of(h);
                ASSERT_NE(e, nullptr);
                kernel().current(kickos_kernel_core()) = e;
                run_exit(0);
                kernel().current(kickos_kernel_core()) = c;
                for (int d = 0; d < FIXTURE_DOMAIN_SLOTS; d++)
                {
                    if (g_domain_refs[d] == 0)
                    {
                        g_domain_live[d] = true;
                    }
                }
            }
        }

        TEST_F(ProgramCreate, an_instance_declared_with_no_restart_nor_on_request_starts_once)
        {
            creator(AUTH_TASKS, 0, 0);
            ASSERT_TRUE(kernel().threads.is_root(sched::current()));
            kos_task_t h = KOS_TASK_NONE;
            ASSERT_EQ(create("main", &h), 0);
            end(h);
            kos_task_t again = KOS_TASK_NONE;
            EXPECT_EQ(create("main", &again), -KOS_EPERM)
                << "a second start of a single-use declaration, its image keeping its .data";
            ASSERT_EQ(create("back", &h), 0);
            end(h);
            EXPECT_EQ(create("back", &again), 0) << "a declaration with restart starts again";
            ASSERT_EQ(create("peer0", &h), 0);
            end(h);
            EXPECT_EQ(create("peer0", &again), 0) << "an on-request declaration starts again";
        }

        TEST_F(ProgramCreate, an_ended_instance_whose_members_still_die_is_not_started_again)
        {
            creator(AUTH_TASKS);
            Thread* const c = sched::current();
            kos_task_t h = KOS_TASK_NONE;
            ASSERT_EQ(create("peer0", &h), 0);
            Thread* const e = entry_of(h);
            ASSERT_NE(e, nullptr);
            ASSERT_EQ(task_slay(h, 0), 0);
            kos_task_t again = KOS_TASK_NONE;
            EXPECT_EQ(create("peer0", &again), -KOS_EAGAIN);
            kernel().current(kickos_kernel_core()) = e;
            run_exit(0);
            kernel().current(kickos_kernel_core()) = c;
            EXPECT_EQ(create("peer0", &again), 0);
        }

        // The transitional calls and fields are gone: the numbers answer as unknown calls, and the
        // spawn parameters name no task and no data block.
        TEST_F(ProgramCreate, the_calls_of_the_transitional_task_model_are_unknown_syscalls)
        {
            creator(AUTH_TASKS);
            for (uintptr_t const nr : {29u, 51u, 64u})
            {
                EXPECT_EQ(static_cast<int32_t>(syscall_dispatch(nr, 0, 0, 0, 0)), -KOS_EINVAL) << nr;
            }
        }
    }
}
