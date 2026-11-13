// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kos_task_create over a packaged driver's declaration: the kernel claims each declared line into
// the entry thread's table at the index the declaration names, bound to the notification it names
// on the line's own bit, and refuses before anything is spent what it could not apply. A declared
// line asks its creator for KOS_AUTH_IRQ whichever task it declares. A claim refused once the entry
// thread exists ends the task, which gives back what the create took. A console driver's entry
// thread holds its grants from its spawn to its start, and a creator that dies between the two ends
// the task.

#include "../common/seam_pools.h"

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/notify.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/image.h>

#include <stddef.h>
#include <string.h>

#include <vector>

#include "kseam_test.h"
#include "region_seam.h"
#include "stub_seam.h"
#include "syscall_internal.h"

namespace
{
    constexpr uint8_t PRIO_ROOT = 8;
    constexpr uint8_t PRIO_DRIVER = 6;
    constexpr uint16_t LINE = 5;
    constexpr uint16_t LINE_SLOT = KOS_SPAWN_DELEGATED_CAP0 + 1u;

    // The declarations, their grants and their names, one object as the composer emits it.
    struct Decls
    {
        kos_task_decl decl[3];
        kos_decl_grant grant[5];
        char names[16];
    };

    constexpr uint32_t name_at(uint32_t at)
    {
        return static_cast<uint32_t>(offsetof(Decls, names)) + at;
    }

    constexpr uint32_t grant_at(uint32_t k)
    {
        return static_cast<uint32_t>(offsetof(Decls, grant) + k * sizeof(kos_decl_grant));
    }

    // drv, a packaged driver with one line bound to its first delegated capability; peer, an
    // on-request task declaring a line; con, an on-request console driver served through its
    // first delegated capability. Past those, a line con may declare and two lines drv may
    // declare in place of its one.
    Decls const CONTROL = {
        {
            {name_at(0), grant_at(0), 0u, 0u, 0u, 1u, KOS_DECL_DRIVER, PRIO_DRIVER, PRIO_DRIVER, 0u, 1u, KICKOS_SEAM_DECL_BUDGETS},
            {name_at(4), grant_at(1), 0u, 0u, 1024u, 1u, KOS_DECL_ON_REQUEST, PRIO_DRIVER, PRIO_DRIVER, 0u, 0u, KICKOS_SEAM_DECL_BUDGETS},
            {name_at(9), grant_at(2), 0u, 0u, 1024u, 0u,
             KOS_DECL_DRIVER | KOS_DECL_ON_REQUEST | KOS_DECL_CONSOLE, PRIO_DRIVER, PRIO_DRIVER, 1u, 0u, KICKOS_SEAM_DECL_BUDGETS},
        },
        {
            {KOS_DECL_LINE, KOS_IRQ_EDGE, LINE, 0u, LINE_SLOT, 0u, 0u},
            {KOS_DECL_LINE, KOS_IRQ_EDGE, LINE + 1u, 0u, 0u, 0u, 0u},
            {KOS_DECL_LINE, KOS_IRQ_EDGE, LINE + 2u, 0u, LINE_SLOT + 1u, 0u, 0u},
            {KOS_DECL_LINE, KOS_IRQ_EDGE, LINE, 0u, LINE_SLOT, 0u, 0u},
            {KOS_DECL_LINE, KOS_IRQ_EDGE, LINE + 3u, 0u, LINE_SLOT + 1u, 0u, 0u},
        },
        "drv\0peer\0con",
    };
    Decls g_decls;

    void decl_entry(void*) {}

    // What a create names, in a region its caller holds.
    struct alignas(kickos::testfix::REGION_MIN) Block
    {
        kos_task_params p;
        kos_thread_params racer;
        kos_cap_grant caps[2];
        uint16_t dest[2];
        kos_window window;
        char strings[32];
        uint32_t out;
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
            constexpr int PROBE_UNRUN = 1;

            // What the handover probe saw: the console task's entry thread not yet started, and a
            // thread spawn's answer for the declared device.
            bool g_saw_pending = false;
            int g_racer_rc = PROBE_UNRUN;
            uint32_t g_prog = KCAP_INVALID;

            bool console_entry_pending()
            {
                IrqLock lock;
                for (Thread const& th : kernel().threads.all())
                {
                    if (th.task_role == TASK_ROLE_ENTRY and th.task != nullptr
                        and task_decl_of(th.task) == 2 and th.state == ThreadState::INACTIVE)
                    {
                        return true;
                    }
                }
                return false;
            }

            // The creator spawns a thread on the device its console driver declares.
            void spawn_on_the_device()
            {
                g_saw_pending = console_entry_pending();
                g_block.window = {STUB_DEVICE_BASE, 0x100u, KOS_WINDOW_DEVICE, 0u};
                g_block.racer = {};
                g_block.racer.entry = decl_entry;
                g_block.racer.prio = PRIO_DRIVER;
                g_block.racer.windows = &g_block.window;
                g_block.racer.window_count = 1;
                g_racer_rc = thread_create_call(&g_block.racer,
                                                reinterpret_cast<uintptr_t>(&g_block.out));
            }

            // The creator's resume is claimed by a slay, as a switch into it redirects it.
            void slay_the_creator()
            {
                g_saw_pending = console_entry_pending();
                (void)thread_cancel_escalate(sched::current(), CANCEL_SLAY);
                kickos_thread_slay_exit(nullptr);
            }

            void create_con()
            {
                (void)task_create_call(g_prog, reinterpret_cast<uintptr_t>(&g_block.p), 0,
                                       reinterpret_cast<uintptr_t>(&g_block.out));
            }

            class DriverCreate : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    g_arena_used = 0;
                    g_decls = CONTROL;
                    declare(g_decls.decl, 3);
                    program("app", reinterpret_cast<uintptr_t>(&decl_entry));
                    g_block = {};
                    g_handover_probe = nullptr;
                    g_saw_pending = false;
                    g_racer_rc = PROBE_UNRUN;
                }

                // The init, root's thread, holding `authority`, the program and a notification.
                void root(uint32_t authority)
                {
                    Kernel& k = kernel();
                    Thread* const c = seat_pool(0, PRIO_ROOT);
                    ASSERT_TRUE(k.threads.is_root(c));
                    join_task(c, task(0));
                    attach_caps(c, KICKOS_SEAM_CAP_WIDTH);
                    cap_seat_authority(c, authority);
                    k.current(kickos_kernel_core()) = c;
                    ASSERT_TRUE(c->mpu.add(reinterpret_cast<uintptr_t>(&g_block), sizeof(g_block),
                                           ARCH_MPU_R | ARCH_MPU_W));
                    ASSERT_EQ(program_find_call(reinterpret_cast<uintptr_t>(text("app", 0)), &prog_), 0);
                    ASSERT_EQ(notify_create(c, &note_), 0);
                    g_block.caps[0] = {note_, KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER};
                    g_block.p.caps = g_block.caps;
                    g_block.p.cap_count = 1;
                }

                int create(char const* instance, kos_task_t* out)
                {
                    g_block.p.instance = text(instance, 16);
                    g_block.out = KOS_TASK_NONE;
                    int const rc = task_create_call(prog_, reinterpret_cast<uintptr_t>(&g_block.p),
                                                    0, reinterpret_cast<uintptr_t>(&g_block.out));
                    *out = g_block.out;
                    return rc;
                }

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

                void refused(char const* instance, int rc, char const* what)
                {
                    uint32_t const holds = kernel().task_holds;
                    kos_task_t h = KOS_TASK_NONE;
                    EXPECT_EQ(create(instance, &h), rc) << what;
                    EXPECT_EQ(h, KOS_TASK_NONE) << what;
                    EXPECT_EQ(kernel().task_holds, holds) << what << ": a refused create spent a task";
                }

                // The endpoint the console declaration names ready, seated with `rights` as the
                // creator's capability at the first delegated position.
                void console_endpoint(uint8_t rights)
                {
                    Kernel& k = kernel();
                    Thread* const c = sched::current();
                    Endpoint* const ep = endpoint();
                    int const handle = k.endpoints.handle_for(k.endpoints.index_of(ep));
                    uint32_t const at = KICKOS_CAP_FIRST_DYNAMIC + 4u;
                    ASSERT_NE(cap_install_at(c, at, handle, CapType::CAP_ENDPOINT, rights, KCAP_BADGE_NONE),
                              nullptr);
                    ASSERT_TRUE(obj_ref_inc(CapType::CAP_ENDPOINT, handle, rights));
                    g_block.caps[0] = {at, rights};
                }

                // The console endpoint, its slot taken and freed until the top bit of its
                // generation is set, so its handle is negative.
                void console_endpoint_negative(uint8_t rights)
                {
                    Kernel& k = kernel();
                    Endpoint* const ep = endpoint();
                    int const slot = k.endpoints.index_of(ep);
                    int others[KICKOS_SEAM_ENDPOINTS];
                    int n = 0;
                    while (true)
                    {
                        int const i = k.endpoints.alloc(nullptr);
                        if (i < 0)
                        {
                            break;
                        }
                        others[n++] = i;
                    }
                    k.endpoints.free(k.endpoints.handle_for(slot));
                    for (uint32_t i = 1; i < 0x8000u; i++)
                    {
                        ASSERT_EQ(k.endpoints.alloc(nullptr), slot);
                        k.endpoints.free(k.endpoints.handle_for(slot));
                    }
                    console_endpoint(rights);
                    for (int i = 0; i < n; i++)
                    {
                        k.endpoints.free(k.endpoints.handle_for(others[i]));
                    }
                    ASSERT_LT(cap_lookup(sched::current(), g_block.caps[0].source_cap)->obj, 0)
                        << "fixture: the handle is not negative";
                }

                // Every free binding slot taken but `left`, so a claim past those finds the pool
                // full.
                void fill_bindings(int left)
                {
                    Kernel& k = kernel();
                    while (true)
                    {
                        int const i = k.irq_bindings.alloc(nullptr);
                        if (i < 0)
                        {
                            break;
                        }
                        fill_.push_back(k.irq_bindings.handle_for(i));
                    }
                    for (int n = 0; n < left and not fill_.empty(); n++)
                    {
                        k.irq_bindings.free(fill_.back());
                        fill_.pop_back();
                    }
                }

                void unfill_bindings()
                {
                    for (int const h : fill_)
                    {
                        kernel().irq_bindings.free(h);
                    }
                    fill_.clear();
                }

                // The entry thread a refused create left behind, slain, and none other.
                Thread* slain_entry()
                {
                    Thread* found = nullptr;
                    for (Thread& th : kernel().threads.all())
                    {
                        if (th.task_role == kickos::TASK_ROLE_ENTRY and th.task != nullptr
                            and th.cancel_kind == CANCEL_SLAY)
                        {
                            EXPECT_EQ(found, nullptr) << "two slain entry threads";
                            found = &th;
                        }
                    }
                    return found;
                }

                // Runs `t` to its exit, the creator current again after.
                void end(Thread* t)
                {
                    Thread* const c = sched::current();
                    kernel().current(kickos_kernel_core()) = t;
                    run_exit(0);
                    kernel().current(kickos_kernel_core()) = c;
                }

                bool line_free(uint16_t line)
                {
                    IrqLock lock;
                    return irq_declared_admit(line, KOS_IRQ_EDGE, nullptr, false) == 0;
                }

                uint32_t prog_ = KCAP_INVALID;
                uint32_t note_ = KCAP_INVALID;
                std::vector<int> fill_;
            };
        }

        TEST_F(DriverCreate, a_declared_line_is_claimed_into_the_entry_thread_bound_to_the_named_notification)
        {
            root(AUTH_TASKS | AUTH_IRQ);
            kos_task_t h = KOS_TASK_NONE;
            ASSERT_EQ(create("drv", &h), 0);
            Thread* const e = entry_of(h);
            ASSERT_NE(e, nullptr);
            IrqLock lock;
            CapEntry const* const line = cap_lookup(e, LINE_SLOT);
            ASSERT_NE(line, nullptr);
            EXPECT_EQ(static_cast<CapType>(line->type), CapType::CAP_IRQ);
            EXPECT_EQ(line->rights, CAP_WAIT | CAP_SIGNAL | CAP_TRANSFER);
            IrqBinding const* const b = kernel().irq_bindings.resolve(static_cast<int>(line->obj));
            ASSERT_NE(b, nullptr);
            EXPECT_EQ(b->line, LINE);
            EXPECT_EQ(b->badge, 0u) << "line 0 raises bit 0";
            int err = 0;
            EXPECT_EQ(b->notify, static_cast<Notification*>(
                                     cap_resolve_e(sched::current(), note_, CapType::CAP_NOTIFY, 0, &err)));
            EXPECT_EQ(static_cast<CapType>(cap_lookup(e, KOS_SPAWN_DELEGATED_CAP0)->type), CapType::CAP_NOTIFY)
                << "the notification delegated where its create placed it";
        }

        TEST_F(DriverCreate, a_line_taken_already_is_refused_before_anything_is_spent)
        {
            root(AUTH_TASKS | AUTH_IRQ);
            kos_task_t first = KOS_TASK_NONE;
            ASSERT_EQ(create("drv", &first), 0);
            g_decls.decl[1].flags = KOS_DECL_DRIVER;
            g_decls.decl[1].notify = 1u;
            g_decls.grant[1].line = LINE;
            g_decls.grant[1].slot = LINE_SLOT;
            refused("peer", -KOS_EBUSY, "the line drv holds");
        }

        TEST_F(DriverCreate, a_driver_s_windows_are_its_declaration_s_alone)
        {
            root(AUTH_TASKS | AUTH_IRQ);
            g_block.window = {reinterpret_cast<uintptr_t>(&g_block), 64u, KOS_WINDOW_MEMORY, 0u};
            g_block.p.windows = &g_block.window;
            g_block.p.window_count = 1;
            refused("drv", -KOS_EINVAL, "a window its creator passes");
        }

        TEST_F(DriverCreate, a_line_slot_a_delegated_capability_lands_on_is_refused)
        {
            root(AUTH_TASKS | AUTH_IRQ);
            g_block.caps[1] = g_block.caps[0];
            g_block.p.cap_count = 2;
            refused("drv", -KOS_EINVAL, "the second capability's default place");
            g_block.p.cap_count = 1;
            g_block.dest[0] = LINE_SLOT;
            g_block.p.cap_dest = g_block.dest;
            refused("drv", -KOS_EINVAL, "a place its creator names");
            g_block.p.cap_dest = nullptr;
            g_decls.grant[0].slot = 0u;
            refused("drv", -KOS_EINVAL, "a reserved index");
        }

        TEST_F(DriverCreate, lines_bound_to_no_notification_are_refused)
        {
            root(AUTH_TASKS | AUTH_IRQ);
            g_decls.decl[0].notify = 0u;
            refused("drv", -KOS_EINVAL, "no notification named");
            g_decls.decl[0].notify = 2u;
            refused("drv", -KOS_EINVAL, "a position past the capabilities passed");
        }

        TEST_F(DriverCreate, a_declared_line_asks_its_creator_for_the_line_authority)
        {
            root(AUTH_TASKS);
            refused("drv", -KOS_EPERM, "a driver's line");
            refused("peer", -KOS_EPERM, "an on-request task's line");
        }

        // The gate is the declaration's flag: no argument of the call names a console.
        TEST_F(DriverCreate, a_console_declaration_asks_its_creator_for_the_console_authority)
        {
            root(AUTH_TASKS);
            console_endpoint(CAP_SIGNAL | CAP_TRANSFER | CAP_HANDOUT);
            uint32_t const begun = g_handovers_begun;
            refused("con", -KOS_EPERM, "a console driver");
            EXPECT_EQ(g_handovers_begun, begun) << "a handover began";
            EXPECT_EQ(cap_console_endpoint(), nullptr) << "a console was published";
        }

        TEST_F(DriverCreate, a_creator_holding_the_console_authority_publishes_the_console)
        {
            root(AUTH_TASKS | AUTH_CONSOLE);
            console_endpoint(CAP_SIGNAL | CAP_TRANSFER | CAP_HANDOUT);
            uint32_t const done = g_handovers_done;
            kos_task_t h = KOS_TASK_NONE;
            ASSERT_EQ(create("con", &h), 0);
            EXPECT_NE(h, KOS_TASK_NONE);
            EXPECT_EQ(g_handovers_done, done + 1u);
            EXPECT_NE(cap_console_endpoint(), nullptr);
        }

        // A handle whose generation has its top bit set reads as a negative int.
        TEST_F(DriverCreate, a_ready_endpoint_with_a_negative_handle_is_seated)
        {
            root(AUTH_TASKS | AUTH_CONSOLE);
            console_endpoint_negative(CAP_SIGNAL | CAP_TRANSFER | CAP_HANDOUT);
            kos_task_t h = KOS_TASK_NONE;
            ASSERT_EQ(create("con", &h), 0);
            IrqLock lock;
            Task const* const t = task_resolve(h);
            ASSERT_NE(t, nullptr);
            Endpoint* const ep = cap_console_endpoint();
            ASSERT_NE(ep, nullptr);
            Kernel& k = kernel();
            EXPECT_EQ(t->watch_ep, notify_bound_store(k.endpoints.handle_for(k.endpoints.index_of(ep))))
                << "the task's ready endpoint was not seated";
        }

        // The console is published before the lines are claimed: a refused claim past the publish
        // ends the task, and the console with it, and leaves the kernel's own console behind.
        TEST_F(DriverCreate, a_line_claim_refused_past_the_console_publish_ends_the_task_and_its_console)
        {
            root(AUTH_TASKS | AUTH_IRQ | AUTH_CONSOLE);
            kos_cap_grant const note = g_block.caps[0];
            console_endpoint(CAP_SIGNAL | CAP_TRANSFER | CAP_HANDOUT);
            g_block.caps[1] = note;
            g_block.p.cap_count = 2;
            g_decls.decl[2].grants = grant_at(2);
            g_decls.decl[2].grant_count = 1u;
            g_decls.decl[2].notify = 2u;
            uint32_t const begun = g_handovers_begun;
            uint32_t const done = g_handovers_done;
            fill_bindings(0);

            refused("con", -KOS_ENOMEM, "a line claim past the publish");

            EXPECT_EQ(g_handovers_begun, begun + 1u) << "the refusal came before the publish";
            EXPECT_EQ(g_handovers_done, done + 1u) << "the console was not handed back";
            Endpoint* const ep = cap_console_endpoint();
            ASSERT_NE(ep, nullptr);
            Thread* const e = slain_entry();
            ASSERT_NE(e, nullptr) << "the refused task's entry thread is not slain";
            unfill_bindings();
            end(e);
            EXPECT_EQ(ep->console, EP_CONSOLE_ENDED) << "the console outlives its refused task";
            kos_task_t h = KOS_TASK_NONE;
            EXPECT_EQ(create("con", &h), 0) << "the declaration starts again once its task is dead";
        }

        // A refusal past the first line claim gives that line back with the task's end, and spends
        // no start of a declaration that starts once.
        TEST_F(DriverCreate, a_second_line_refused_gives_the_first_back_and_spends_no_start)
        {
            root(AUTH_TASKS | AUTH_IRQ);
            g_decls.decl[0].grants = grant_at(3);
            g_decls.decl[0].grant_count = 2u;
            fill_bindings(1);

            refused("drv", -KOS_ENOMEM, "the second line's claim");

            Thread* const e = slain_entry();
            ASSERT_NE(e, nullptr) << "the refused task's entry thread is not slain";
            EXPECT_FALSE(line_free(LINE)) << "fixture: the first line was not claimed";
            end(e);
            EXPECT_TRUE(line_free(LINE)) << "the first line outlives the refused task";
            EXPECT_TRUE(line_free(LINE + 3u));
            unfill_bindings();
            kos_task_t h = KOS_TASK_NONE;
            EXPECT_EQ(create("drv", &h), 0) << "the refused create spent the declaration's one start";
        }

        TEST_F(DriverCreate, a_console_driver_s_device_is_held_before_its_entry_thread_starts)
        {
            root(AUTH_TASKS | AUTH_CONSOLE | AUTH_MEMORY);
            console_endpoint(CAP_SIGNAL | CAP_TRANSFER | CAP_HANDOUT);
            g_decls.decl[2].grants = grant_at(2);
            g_decls.decl[2].grant_count = 1u;
            g_decls.grant[2] = {KOS_DECL_DEVICE, 0u, 0u, 0u, 0u, 0x100u, STUB_DEVICE_BASE};
            g_handover_probe = spawn_on_the_device;
            kos_task_t h = KOS_TASK_NONE;
            ASSERT_EQ(create("con", &h), 0);
            EXPECT_TRUE(g_saw_pending) << "fixture: the probe ran outside the handover";
            EXPECT_EQ(g_racer_rc, -KOS_EBUSY) << "a second thread was granted the driver's device";
        }

        // A declaration that starts once: its entry thread never ran.
        TEST_F(DriverCreate, a_creator_slain_before_its_console_driver_starts_ends_the_task)
        {
            root(AUTH_TASKS | AUTH_CONSOLE);
            console_endpoint(CAP_SIGNAL | CAP_TRANSFER | CAP_HANDOUT);
            g_decls.decl[2].flags = KOS_DECL_DRIVER | KOS_DECL_CONSOLE;
            g_block.p.instance = text("con", 16);
            g_prog = prog_;
            // The creator and a thread that outlives the arm.
            kernel().live = 2;
            g_handover_probe = slay_the_creator;
            run_noreturn(create_con);
            EXPECT_TRUE(g_saw_pending) << "fixture: the probe ran outside the handover";
            Thread* const e = slain_entry();
            ASSERT_NE(e, nullptr) << "the unstarted entry thread is not slain";
            EXPECT_NE(e->state, ThreadState::INACTIVE) << "the entry thread was never started";
            end(e);
            IrqLock lock;
            EXPECT_EQ(task_decl_state(2), 0) << "the declaration never starts again";
        }
    }
}
