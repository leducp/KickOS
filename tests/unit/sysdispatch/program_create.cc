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
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>
#include <kickos/time.h>

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

    // What a create's park saw, and the last member of the dying instance it waits on.
    struct Dying
    {
        kickos::Task* record;
        kickos::Thread* member;
        kickos::Thread* creator;
        uint8_t kind;
        void* obj;
        uint64_t deadline_ns;
        bool woken;
    };
    Dying g_dying;

    void seen(kickos::Thread* parked)
    {
        g_dying.kind = parked->wait_kind;
        g_dying.obj = parked->wait_obj;
        g_dying.deadline_ns = parked->deadline_ns;
    }

    // While the create waits: the member's exit, its sweep done, and exit_current's wake of a
    // task-empty waiter on the dead group.
    void the_member_dies(kickos::Thread* parked)
    {
        using namespace kickos;
        seen(parked);
        {
            IrqLock lock;
            Task* const t = g_dying.record;
            uint16_t const gen = task_gen(t);
            (void)task_release(t, lock);
            task_member_leave(g_dying.member, kernel().threads.index_of(g_dying.member));
            if (task_sweep_done(t))
            {
                if (parked->wait_task_target() == t)
                {
                    g_dying.woken = true;
                    thread_abort_park(parked, 0, lock);
                }
                task_report_death(t, gen);
            }
        }
        kernel().current(kickos_kernel_core()) = g_dying.creator;
    }

    // What the death wait's park lent, read where it switched to.
    struct Lent
    {
        kickos::Thread* ran;
        uint8_t member_prio;
        uint8_t owner_prio;
        uint8_t member_recomputed;
    };
    Lent g_lent;
    kickos::Thread* g_owner;

    void lent(kickos::Thread* parked)
    {
        using namespace kickos;
        seen(parked);
        g_lent.ran = sched::current();
        g_lent.member_prio = g_dying.member->prio;
        g_lent.member_recomputed = thread_effective_prio(g_dying.member);
        if (g_owner != nullptr)
        {
            g_lent.owner_prio = g_owner->prio;
        }
    }

    // The member runs the rest of its exit only where the switch chose it over the spinner;
    // otherwise the spinner keeps the CPU until the wait's deadline.
    void the_member_dies_if_it_runs(kickos::Thread* parked)
    {
        using namespace kickos;
        lent(parked);
        if (g_lent.ran == g_dying.member)
        {
            the_member_dies(parked);
            return;
        }
        {
            IrqLock lock;
            thread_abort_park(parked, -KOS_ETIMEDOUT, lock);
        }
        kernel().current(kickos_kernel_core()) = g_dying.creator;
    }

    void the_deadline_passes_lent(kickos::Thread* parked)
    {
        lent(parked);
        {
            kickos::IrqLock lock;
            kickos::thread_abort_park(parked, -KOS_ETIMEDOUT, lock);
        }
        kickos::kernel().current(kickos_kernel_core()) = g_dying.creator;
    }

    void the_creator_is_killed_lent(kickos::Thread* parked)
    {
        lent(parked);
        {
            kickos::IrqLock lock;
            kickos::thread_cancel_kind(parked, kickos::CANCEL_KILL, lock);
        }
        kickos::kernel().current(kickos_kernel_core()) = g_dying.creator;
    }

    // A second thread already waits on the same death when the create parks; the two leave in
    // the order `second_first` names.
    struct TwoWaiters
    {
        kickos::Thread* second;
        bool second_first;
        uint8_t with_both;
        uint8_t with_one;
    };
    TwoWaiters g_two;

    void two_waiters_leave(kickos::Thread* parked)
    {
        using namespace kickos;
        IrqLock lock;
        g_two.with_both = g_dying.member->prio;
        Thread* first = parked;
        Thread* last = g_two.second;
        if (g_two.second_first)
        {
            first = g_two.second;
            last = parked;
        }
        thread_abort_park(first, -KOS_ETIMEDOUT, lock);
        g_two.with_one = g_dying.member->prio;
        thread_abort_park(last, -KOS_ETIMEDOUT, lock);
        kernel().current(kickos_kernel_core()) = g_dying.creator;
    }

    void the_deadline_passes(kickos::Thread* parked)
    {
        using namespace kickos;
        seen(parked);
        {
            IrqLock lock;
            thread_abort_park(parked, -KOS_ETIMEDOUT, lock);
        }
        kernel().current(kickos_kernel_core()) = g_dying.creator;
    }

    void the_creator_is_killed(kickos::Thread* parked)
    {
        using namespace kickos;
        seen(parked);
        {
            IrqLock lock;
            thread_cancel_kind(parked, CANCEL_KILL, lock);
        }
        kernel().current(kickos_kernel_core()) = g_dying.creator;
    }

    // What a create names, in a region its caller holds.
    struct alignas(kickos::testfix::REGION_MIN) Block
    {
        kos_task_params p;
        kos_thread_params spawn;
        char const* argv[KICKOS_MAX_ARGC + 2];
        char strings[64];
        uint32_t lock_word;
        kos_task_t out;
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

                int create(char const* instance, kos_task_t* out, uint32_t timeout_us = 0)
                {
                    g_block.p.instance = nullptr;
                    if (instance != nullptr)
                    {
                        g_block.p.instance = text(instance, 48);
                    }
                    g_block.out = KOS_TASK_NONE;
                    int const rc = task_create_call(prog_, reinterpret_cast<uintptr_t>(&g_block.p),
                                                    timeout_us,
                                                    reinterpret_cast<uintptr_t>(&g_block.out));
                    *out = g_block.out;
                    return rc;
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

        TEST_F(ProgramCreate, a_start_with_a_timeout_waits_out_the_dying_instance_then_starts_it)
        {
            creator(AUTH_TASKS);
            Thread* const c = sched::current();
            kos_task_t h = KOS_TASK_NONE;
            ASSERT_EQ(create("peer0", &h), 0);
            Thread* const e = entry_of(h);
            ASSERT_NE(e, nullptr);
            Task* record = nullptr;
            {
                IrqLock lock;
                record = task_resolve(h);
            }
            ASSERT_EQ(task_slay(h, 0), 0);
            kos_task_t again = KOS_TASK_NONE;
            ASSERT_EQ(create("peer0", &again), -KOS_EAGAIN) << "fixture: peer0 is not dying";
            uint32_t const holds = kernel().task_holds;
            g_dying = Dying{record, e, c, WAIT_NONE, nullptr, 1u, false};
            wake_next_park(the_member_dies);
            EXPECT_EQ(create("peer0", &again, KOS_TIMEOUT_NONE), 0);
            EXPECT_EQ(g_dying.kind, WAIT_TASK_EMPTY) << "the wait is not the death's own park";
            EXPECT_EQ(g_dying.obj, record) << "the wait parks on another task than the dying one";
            EXPECT_EQ(g_dying.deadline_ns, 0u) << "a wait with no bound armed a deadline";
            EXPECT_TRUE(g_dying.woken) << "the last sweep's wake does not find the waiter";
            ASSERT_NE(again, KOS_TASK_NONE);
            EXPECT_EQ(handle_index(again), handle_index(h)) << "the start took another record";
            EXPECT_STREQ(entry_of(again)->name, "peer0");
            EXPECT_EQ(kernel().task_holds, holds + 1u);
        }

        TEST_F(ProgramCreate, a_start_with_no_instance_named_waits_out_the_one_still_dying)
        {
            creator(AUTH_TASKS | AUTH_IRQ);
            Thread* const c = sched::current();
            kos_task_t first = KOS_TASK_NONE;
            kos_task_t second = KOS_TASK_NONE;
            ASSERT_EQ(create(nullptr, &first), 0);
            ASSERT_EQ(create(nullptr, &second), 0);
            Thread* const e = entry_of(first);
            ASSERT_NE(e, nullptr);
            Task* record = nullptr;
            {
                IrqLock lock;
                record = task_resolve(first);
            }
            ASSERT_EQ(task_slay(first, 0), 0);
            g_dying = Dying{record, e, c, WAIT_NONE, nullptr, 0u, false};
            wake_next_park(the_deadline_passes);
            kos_task_t h = KOS_TASK_NONE;
            EXPECT_EQ(create(nullptr, &h, 1000u), -KOS_ETIMEDOUT) << "the create did not wait";
            EXPECT_EQ(g_dying.kind, WAIT_TASK_EMPTY);
            EXPECT_EQ(g_dying.obj, record) << "the wait is not on the instance still dying";
        }

        TEST_F(ProgramCreate, a_wait_on_a_start_ends_at_its_timeout_with_nothing_started)
        {
            creator(AUTH_TASKS);
            Thread* const c = sched::current();
            kos_task_t h = KOS_TASK_NONE;
            ASSERT_EQ(create("peer0", &h), 0);
            ASSERT_EQ(task_slay(h, 0), 0);
            uint32_t const holds = kernel().task_holds;
            g_dying = Dying{nullptr, nullptr, c, WAIT_NONE, nullptr, 0u, false};
            wake_next_park(the_deadline_passes);
            uint64_t const before = ktime_now();
            kos_task_t again = KOS_TASK_NONE;
            EXPECT_EQ(create("peer0", &again, 1000u), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_dying.kind, WAIT_TASK_EMPTY);
            EXPECT_GE(g_dying.deadline_ns, before + 1000000u) << "the deadline is not the timeout's";
            EXPECT_EQ(again, KOS_TASK_NONE);
            EXPECT_EQ(kernel().task_holds, holds) << "a create that timed out spent a task";
            EXPECT_EQ(c->wait_kind, WAIT_NONE);
        }

        TEST_F(ProgramCreate, a_cancel_ends_a_wait_on_a_start_with_nothing_started)
        {
            creator(AUTH_TASKS);
            Thread* const c = sched::current();
            kos_task_t h = KOS_TASK_NONE;
            ASSERT_EQ(create("peer0", &h), 0);
            ASSERT_EQ(task_slay(h, 0), 0);
            uint32_t const holds = kernel().task_holds;
            g_dying = Dying{nullptr, nullptr, c, WAIT_NONE, nullptr, 0u, false};
            wake_next_park(the_creator_is_killed);
            kos_task_t again = KOS_TASK_NONE;
            EXPECT_EQ(create("peer0", &again, KOS_TIMEOUT_NONE), -KOS_ECANCELED);
            EXPECT_EQ(g_dying.kind, WAIT_TASK_EMPTY);
            EXPECT_EQ(again, KOS_TASK_NONE);
            EXPECT_EQ(kernel().task_holds, holds) << "a cancelled create spent a task";
            EXPECT_EQ(c->wait_kind, WAIT_NONE) << "the cancel left the wait edge";
        }

        // A creator above a medium thread that keeps the CPU waits on a member below both: the
        // member runs its death out only on the creator's priority.
        class DeathLend : public ProgramCreate
        {
          protected:
            static constexpr uint8_t PRIO_SPIN = 7;
            static constexpr uint8_t PRIO_OWNER = 5;
            static constexpr int SLOT_SPIN = 6;
            static constexpr int SLOT_OWNER = 7;

            // peer0 slain with its entry still to run its exit, and a spinner ready.
            void dying_with_spinner()
            {
                creator(AUTH_TASKS);
                c_ = sched::current();
                ASSERT_EQ(create("peer0", &h_), 0);
                e_ = entry_of(h_);
                ASSERT_NE(e_, nullptr);
                {
                    IrqLock lock;
                    record_ = task_resolve(h_);
                }
                ASSERT_EQ(task_slay(h_, 0), 0);
                spin_ = seat_pool(SLOT_SPIN, PRIO_SPIN);
                g_dying = Dying{record_, e_, c_, WAIT_NONE, nullptr, 0u, false};
                g_lent = Lent{nullptr, 0u, 0u, 0u};
                g_owner = nullptr;
                ASSERT_EQ(e_->prio, PRIO_PEER);
                ASSERT_LT(PRIO_PEER, PRIO_SPIN);
                ASSERT_LT(PRIO_SPIN, PRIO_CREATOR);
            }

            Thread* c_ = nullptr;
            Thread* e_ = nullptr;
            Thread* spin_ = nullptr;
            Task* record_ = nullptr;
            kos_task_t h_ = KOS_TASK_NONE;
        };

        TEST_F(DeathLend, a_wait_on_a_start_lends_its_priority_so_the_dying_member_outruns_a_spinner)
        {
            dying_with_spinner();
            wake_next_park(the_member_dies_if_it_runs);
            kos_task_t again = KOS_TASK_NONE;
            EXPECT_EQ(create("peer0", &again, 1000u), 0) << "the spinner starved the death";
            EXPECT_EQ(g_lent.ran, e_) << "the switch away from the waiter chose another thread";
            EXPECT_EQ(g_lent.member_prio, PRIO_CREATOR);
            EXPECT_EQ(g_lent.member_recomputed, PRIO_CREATOR)
                << "a recompute of the member's priority drops the lend";
        }

        TEST_F(DeathLend, a_wait_on_a_slay_lends_its_priority_to_the_member_it_stops)
        {
            creator(AUTH_TASKS);
            c_ = sched::current();
            ASSERT_EQ(create("peer0", &h_), 0);
            e_ = entry_of(h_);
            ASSERT_NE(e_, nullptr);
            spin_ = seat_pool(SLOT_SPIN, PRIO_SPIN);
            g_dying = Dying{nullptr, e_, c_, WAIT_NONE, nullptr, 0u, false};
            g_lent = Lent{nullptr, 0u, 0u, 0u};
            g_owner = nullptr;
            wake_next_park(the_deadline_passes_lent);
            EXPECT_EQ(task_slay(h_, 1000u), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_dying.kind, WAIT_TASK_EMPTY);
            EXPECT_EQ(g_lent.ran, e_) << "the slay's park switched to the spinner";
            EXPECT_EQ(g_lent.member_prio, PRIO_CREATOR);
            EXPECT_EQ(e_->prio, PRIO_PEER) << "the slay's timeout left the member lent to";
        }

        TEST_F(DeathLend, a_timeout_ends_the_lend)
        {
            dying_with_spinner();
            wake_next_park(the_deadline_passes_lent);
            kos_task_t again = KOS_TASK_NONE;
            EXPECT_EQ(create("peer0", &again, 1000u), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_lent.member_prio, PRIO_CREATOR) << "fixture: nothing was lent";
            EXPECT_EQ(e_->prio, PRIO_PEER) << "the member keeps the priority of a waiter gone";
            EXPECT_TRUE(kernel().death_waiters.empty());
        }

        TEST_F(DeathLend, a_cancel_ends_the_lend)
        {
            dying_with_spinner();
            wake_next_park(the_creator_is_killed_lent);
            kos_task_t again = KOS_TASK_NONE;
            EXPECT_EQ(create("peer0", &again, KOS_TIMEOUT_NONE), -KOS_ECANCELED);
            EXPECT_EQ(g_lent.member_prio, PRIO_CREATOR) << "fixture: nothing was lent";
            EXPECT_EQ(e_->prio, PRIO_PEER) << "the member keeps the priority of a waiter gone";
            EXPECT_TRUE(kernel().death_waiters.empty());
        }

        TEST_F(DeathLend, the_lend_passes_through_a_member_parked_on_a_mutex_and_back)
        {
            dying_with_spinner();
            static uint32_t word = 0;
            Thread* const owner = seat_pool(SLOT_OWNER, PRIO_OWNER);
            WordRecord* m = nullptr;
            {
                IrqLock lock;
                m = own_mutex(owner, &word);
                park_mutex_waiter(e_, m);
                sched::set_prio(owner, thread_effective_prio(owner), lock);
            }
            ASSERT_EQ(owner->prio, PRIO_PEER) << "fixture: the member lends the owner nothing";
            g_owner = owner;
            wake_next_park(the_deadline_passes_lent);
            kos_task_t again = KOS_TASK_NONE;
            EXPECT_EQ(create("peer0", &again, 1000u), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_lent.member_prio, PRIO_CREATOR);
            EXPECT_EQ(g_lent.owner_prio, PRIO_CREATOR) << "the lend stops at the member's mutex";
            EXPECT_EQ(owner->prio, PRIO_PEER) << "the owner keeps a lend the waiter took back";
        }

        TEST_F(DeathLend, a_lend_crosses_every_death_and_mutex_edge_and_comes_back)
        {
            dying_with_spinner();
            // peer0's member waits on o1's mutex, o1 on the death of t2, whose member waits on
            // o2's mutex, o2 on the death of t3: two death edges past the creator's own.
            static uint32_t w1 = 0;
            static uint32_t w2 = 0;
            Task* const t2 = task(1);
            Task* const t3 = task(2);
            Thread* const o1 = seat_pool(SLOT_OWNER, PRIO_OWNER);
            Thread* const e2 = seat_pool(SLOT_OWNER + 1, 4);
            Thread* const o2 = seat_pool(SLOT_OWNER + 2, 4);
            Thread* const e3 = seat_pool(SLOT_OWNER + 3, 3);
            attach_caps(e2, KICKOS_SEAM_CAP_WIDTH);
            attach_caps(e3, KICKOS_SEAM_CAP_WIDTH);
            join_task(e2, t2);
            join_task(e3, t3);
            {
                IrqLock lock;
                park_mutex_waiter(e_, own_mutex(o1, &w1));
                park_mutex_waiter(e2, own_mutex(o2, &w2));
                park_death_waiter(o1, t2);
                park_death_waiter(o2, t3);
                pi_settle(o1, lock);
                pi_settle(o2, lock);
            }
            uint8_t const e3_before = e3->prio;
            ASSERT_LT(e3_before, PRIO_CREATOR) << "fixture: the far member is lent to already";
            g_owner = e3;
            wake_next_park(the_deadline_passes_lent);
            kos_task_t again = KOS_TASK_NONE;
            EXPECT_EQ(create("peer0", &again, 1000u), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_lent.owner_prio, PRIO_CREATOR)
                << "the lend stops before the second death edge";
            EXPECT_EQ(e3->prio, e3_before) << "the far member keeps a lend the waiter took back";
            IrqLock lock;
            EXPECT_EQ(thread_effective_prio(e3), e3->prio);
        }

        TEST_F(DeathLend, two_waiters_on_one_death_leave_in_either_order)
        {
            for (bool const second_first : {false, true})
            {
                SCOPED_TRACE(second_first);
                if (second_first)
                {
                    TearDown();
                    SetUp();
                }
                dying_with_spinner();
                Thread* const second = seat_pool(SLOT_OWNER, PRIO_SPIN);
                {
                    IrqLock lock;
                    park_death_waiter(second, record_);
                    pi_settle(e_, lock);
                }
                ASSERT_EQ(e_->prio, PRIO_SPIN) << "fixture: the second waiter lends nothing";
                g_two = TwoWaiters{second, second_first, 0u, 0u};
                wake_next_park(two_waiters_leave);
                kos_task_t again = KOS_TASK_NONE;
                EXPECT_EQ(create("peer0", &again, 1000u), -KOS_ETIMEDOUT);
                EXPECT_EQ(g_two.with_both, PRIO_CREATOR);
                if (second_first)
                {
                    EXPECT_EQ(g_two.with_one, PRIO_CREATOR) << "the higher waiter's lend went";
                }
                else
                {
                    EXPECT_EQ(g_two.with_one, PRIO_SPIN) << "the lower waiter's lend did not stand";
                }
                EXPECT_EQ(e_->prio, PRIO_PEER);
                EXPECT_TRUE(kernel().death_waiters.empty());
            }
        }

        // The creator locks a mutex a lent member of the dying task holds: the member takes the
        // creator's priority over the lend, and keeps it while the creator is queued.
        TEST_F(DeathLend, a_contended_lock_raises_a_lent_member_and_the_lend_does_not_undo_it)
        {
            dying_with_spinner();
            Thread* const slayer = seat_pool(SLOT_OWNER, PRIO_SPIN);
            g_block.lock_word = 0;
            {
                IrqLock lock;
                park_death_waiter(slayer, record_);
                pi_settle(e_, lock);
                (void)own_mutex(e_, &g_block.lock_word);
            }
            ASSERT_EQ(e_->prio, PRIO_SPIN) << "fixture: the member is not lent to";
            wake_next_park(the_deadline_passes_lent);
            EXPECT_EQ(mutex_lock(reinterpret_cast<uintptr_t>(&g_block.lock_word), 1000u),
                      -KOS_ETIMEDOUT);
            EXPECT_EQ(g_lent.member_prio, PRIO_CREATOR)
                << "the lend's recompute undid the lock's raise of the holder";
            EXPECT_EQ(e_->prio, PRIO_SPIN) << "the holder keeps the raise of a waiter gone";
        }

        // A death waiter on an unrelated task, whose member the fixture leaves unlent: a lock
        // contended elsewhere recomputes nothing of that task, the walk being the lock's own.
        TEST_F(DeathLend, a_contended_lock_visits_no_task_an_unrelated_death_waiter_awaits)
        {
            dying_with_spinner();
            Task* const other = task(1);
            Thread* const member = seat_pool(SLOT_OWNER, 3);
            attach_caps(member, KICKOS_SEAM_CAP_WIDTH);
            join_task(member, other);
            Thread* const waiter = seat_pool(SLOT_OWNER + 1, PRIO_SPIN);
            Thread* const holder = seat_pool(SLOT_OWNER + 2, PRIO_OWNER);
            g_block.lock_word = 0;
            {
                IrqLock lock;
                park_death_waiter(waiter, other);
                (void)own_mutex(holder, &g_block.lock_word);
            }
            ASSERT_EQ(member->prio, 3) << "fixture: the unrelated member was lent to";
            g_owner = holder;
            wake_next_park(the_deadline_passes_lent);
            EXPECT_EQ(mutex_lock(reinterpret_cast<uintptr_t>(&g_block.lock_word), 1000u),
                      -KOS_ETIMEDOUT);
            EXPECT_EQ(g_lent.owner_prio, PRIO_CREATOR) << "fixture: the lock lent nothing";
            EXPECT_EQ(member->prio, 3) << "the lock recomputed a task nothing it raised awaits";
        }

        // The creator holds a mutex the member it slays waits on: the stop cuts that wait in the
        // slay's own lock hold, so the slay waits the death out rather than refusing.
        TEST_F(DeathLend, a_slay_holding_a_mutex_its_member_waits_on_stops_the_member_and_waits)
        {
            creator(AUTH_TASKS);
            c_ = sched::current();
            ASSERT_EQ(create("peer0", &h_), 0);
            e_ = entry_of(h_);
            ASSERT_NE(e_, nullptr);
            {
                IrqLock lock;
                record_ = task_resolve(h_);
            }
            g_block.lock_word = 0;
            {
                IrqLock lock;
                park_mutex_waiter(e_, own_mutex(c_, &g_block.lock_word));
            }
            g_dying = Dying{record_, e_, c_, WAIT_NONE, nullptr, 0u, false};
            wake_next_park(the_member_dies);
            EXPECT_EQ(task_slay(h_, 1000u), 0) << "a slay its own mutex closed a cycle for";
            EXPECT_EQ(g_dying.kind, WAIT_TASK_EMPTY);
            EXPECT_TRUE(g_dying.woken);
        }

        // peer0's two threads reach a death waiter of t2 directly and through a death waiter of
        // t3: the lend visits each dying task once and every priority settles both ways.
        TEST_F(DeathLend, a_waiter_reached_on_two_paths_is_drained_once_and_settles)
        {
            creator(AUTH_TASKS);
            c_ = sched::current();
            ASSERT_EQ(create("peer0", &h_), 0);
            e_ = entry_of(h_);
            ASSERT_NE(e_, nullptr);
            {
                IrqLock lock;
                record_ = task_resolve(h_);
            }
            Thread* const m2 = seat_pool(SLOT_OWNER + 4, 2);
            attach_caps(m2, KICKOS_SEAM_CAP_WIDTH);
            join_task(m2, record_);
            ASSERT_EQ(task_slay(h_, 0), 0);
            g_dying = Dying{record_, e_, c_, WAIT_NONE, nullptr, 0u, false};
            g_lent = Lent{nullptr, 0u, 0u, 0u};
            static uint32_t w1 = 0;
            static uint32_t w2 = 0;
            static uint32_t w3 = 0;
            Task* const t2 = task(1);
            Task* const t3 = task(2);
            Thread* const a = seat_pool(SLOT_OWNER, 3);
            Thread* const m4 = seat_pool(SLOT_OWNER + 1, 2);
            Thread* const b = seat_pool(SLOT_OWNER + 2, 3);
            Thread* const m3 = seat_pool(SLOT_OWNER + 3, 2);
            for (Thread* const t : {m4, m3})
            {
                attach_caps(t, KICKOS_SEAM_CAP_WIDTH);
            }
            join_task(m4, t2);
            join_task(m3, t3);
            {
                IrqLock lock;
                park_mutex_waiter(e_, own_mutex(a, &w1));
                park_mutex_waiter(m2, own_mutex(b, &w2));
                park_mutex_waiter(m3, own_mutex(a, &w3));
                park_death_waiter(a, t2);
                park_death_waiter(b, t3);
                for (Thread* const t : {e_, m2, m3, m4, a, b})
                {
                    pi_settle(t, lock);
                }
            }
            uint8_t const m4_before = m4->prio;
            ASSERT_LT(m4_before, PRIO_CREATOR) << "fixture: t2's thread is lent to already";
            g_owner = m4;
            wake_next_park(the_deadline_passes_lent);
            kos_task_t again = KOS_TASK_NONE;
            EXPECT_EQ(create("peer0", &again, 1000u), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_lent.owner_prio, PRIO_CREATOR) << "the lend did not reach t2 through t3";
            EXPECT_TRUE(kernel().death_dirty.empty());
            int waiters = 0;
            for (ListNode* n = kernel().death_waiters.head; n != nullptr; n = n->next)
            {
                waiters++;
            }
            EXPECT_EQ(waiters, 2) << "a death waiter was lost or linked twice";
            IrqLock lock;
            for (Thread* const t : {e_, m2, m3, m4, a, b})
            {
                EXPECT_EQ(t->prio, thread_effective_prio(t)) << "thread " << t->id;
            }
            EXPECT_EQ(m4->prio, m4_before);
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
