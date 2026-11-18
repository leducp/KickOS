// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A packaged driver at two kernel cores over the real dispatch, its line released while core
// one's dispatch holds the grace period open: the create answers the retry with no park, the
// INSTANCE watch on the driver is raised only by the reclamation, strictly after the grace, which
// completes with or without a watcher, and the create then starts the driver at once. A user
// task's line, which its creator claims, holds its watch the same way.
// Single-threaded: core one's dispatch runs the arm as core zero from inside its handler.

#include <kickos/irq.h>
#include <kickos/notify.h>
#include <kickos/sync.h>
#include <kickos/task.h>

#include <new>

#include "sys_fixture.h"

#if not KICKOS_PRESYNC or KICKOS_KERNEL_CORES != 2
#error "this gate's posture stages a create's copy outside the lock, at two kernel cores"
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
            uint32_t watch_spec(uint32_t kind, uint32_t events)
            {
                return (kind << KOS_WATCH_KIND_SHIFT) | events;
            }

            constexpr uint16_t LINE = 5;
            constexpr int LINE_CARRIER = 7;
            constexpr uint16_t LINE_SLOT = KOS_SPAWN_DELEGATED_CAP0 + 1u;

            struct DriverDecls
            {
                kos_task_decl decl[1];
                kos_decl_grant grant[1];
                char names[4];
            };

            DriverDecls const DRIVER = {
                {
                    {static_cast<uint32_t>(offsetof(DriverDecls, names)),
                     static_cast<uint32_t>(offsetof(DriverDecls, grant)), 1u, 0u, 1024u, 1u,
                     KOS_DECL_DRIVER | KOS_DECL_ON_REQUEST, MAIN_PRIO, MAIN_PRIO, 0u, 1u,
                     KICKOS_SEAM_DECL_BUDGETS},
                },
                {
                    {KOS_DECL_LINE, KOS_IRQ_EDGE, LINE, 0u, LINE_SLOT, 0u, 0u},
                },
                "drv",
            };

            // A user task the creator claims its line for, which its create does not claim.
            DriverDecls const USER = {
                {
                    {static_cast<uint32_t>(offsetof(DriverDecls, names)),
                     static_cast<uint32_t>(offsetof(DriverDecls, grant)), 1u, 0u, 1024u, 1u,
                     KOS_DECL_ON_REQUEST, MAIN_PRIO, MAIN_PRIO, 0u, 0u, KICKOS_SEAM_DECL_BUDGETS},
                },
                {
                    {KOS_DECL_LINE, KOS_IRQ_EDGE, LINE, 0u, 0u, 0u, 0u},
                },
                "usr",
            };

            constexpr uint32_t WATCH_BIT = 4;

            struct Seen
            {
                bool ran = false;
                int state = 0;
                uint32_t pending = 0;
                int32_t create = 1;
                bool record_retiring = false;
                int64_t armed = 1;
                int claim = 1;
            };

            class SysGraceDrive;
            SysGraceDrive* g_case = nullptr;
            Seen g_seen;

            class SysGraceDrive : public SysFixture
            {
              protected:
                void SetUp() override
                {
                    g_case = this;
                    g_seen = Seen{};
                    SysFixture::SetUp();
                    declare(DRIVER.decl, 1);
                    cap_seat_authority(main_, AUTH_MEMORY | AUTH_TASKS | AUTH_IRQ);
                    // Core one runs a thread of its own over an idle of its own.
                    alignas(Thread) static unsigned char idle_store[sizeof(Thread)];
                    Thread* const peer_idle = new (idle_store) Thread{};
                    peer_idle->base_prio = KICKOS_PRIO_IDLE;
                    peer_idle->prio = KICKOS_PRIO_IDLE;
                    sched::add_idle(peer_idle, core_at(1));
                    Thread* const peer = seat_pool(3, MAIN_PRIO);
                    peer->affinity = 1u << 1;
                    seat_running_on(peer, core_at(1));
                    main_->affinity = 1u;
                    main_note_ = note_of(main_);
                    ASSERT_NE(main_note_, KCAP_INVALID);
                    watch_note_ = note_of(main_);
                    ASSERT_NE(watch_note_, KCAP_INVALID);
                    {
                        IrqLock lock;
                        ASSERT_EQ(notify_badge(main_, watch_note_, WATCH_BIT, &watch_copy_), 0);
                    }
                }

                void TearDown() override
                {
                    {
                        IrqLock lock;
                        for (Domain& d : kernel().domains())
                        {
                            if (d.space != nullptr)
                            {
                                aspace_release(d.space, &d.ranges);
                                d.space = nullptr;
                            }
                        }
                    }
                    SysFixture::TearDown();
                }

                // A create of drv by main, handing over its lines' notification.
                int32_t create_drv(kos_task_t* out)
                {
                    kos_task_params* const p = task_params(MAIN_PARAMS, 0, 0, 0);
                    *host<kos_cap_grant>(uva(MAIN_PARAMS, CAPS_AT)) = {
                        main_note_, KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER};
                    p->caps = reinterpret_cast<kos_cap_grant const*>(uva(MAIN_PARAMS, CAPS_AT));
                    p->cap_count = 1;
                    uint32_t* const o = host<uint32_t>(uva(MAIN_OUT));
                    *o = KOS_TASK_NONE;
                    int32_t const rc = err(call_as(main_, KOS_SYS_TASK_CREATE, program_for(main_),
                                                   uva(MAIN_PARAMS, TASK_PARAMS_AT), uva(MAIN_OUT)));
                    *out = static_cast<kos_task_t>(*o);
                    return rc;
                }

                // Main's INSTANCE watch on drv, raising WATCH_BIT of its watch notification: its
                // handle, or the refusal.
                int64_t watch_drv()
                {
                    return watch_named("drv");
                }

                // The same on the instance `instance` names, of at most three characters.
                int64_t watch_named(char const* instance)
                {
                    char* const name = host<char>(uva(EDIT_PARAMS, PROGRAM_AT));
                    memcpy(name, instance, strlen(instance) + 1u);
                    uint32_t* const o = host<uint32_t>(uva(EDIT_PARAMS, PROGRAM_AT + 8));
                    *o = KCAP_INVALID;
                    int32_t const rc =
                        err(call_as(main_, KOS_SYS_WATCH,
                                    watch_spec(KOS_WATCH_INSTANCE, KOS_INSTANCE_STARTABLE),
                                    program_for(main_), watch_copy_, uva(EDIT_PARAMS, PROGRAM_AT),
                                    uva(EDIT_PARAMS, PROGRAM_AT + 8)));
                    if (rc != 0)
                    {
                        return rc;
                    }
                    return *o;
                }

                uint32_t pending() const
                {
                    IrqLock lock;
                    int e = 0;
                    Notification const* const n = static_cast<Notification*>(
                        cap_resolve_e(main_, watch_note_, CapType::CAP_NOTIFY, 0, &e));
                    return n->pending;
                }

                static uint32_t note_of(Thread* t)
                {
                    uint32_t note = KCAP_INVALID;
                    if (notify_create(t, &note) != 0)
                    {
                        return KCAP_INVALID;
                    }
                    return note;
                }

                // Inside core one's dispatch, as core zero: drv's entry dies holding its line, and
                // what the death leaves is read.
                static void drv_dies(void*)
                {
                    g_core = 0;
                    SysGraceDrive* const c = g_case;
                    running() = c->entry_;
                    run_exit(0);
                    running() = c->main_;
                    int32_t status = 0;
                    g_seen.state = task_state_call(c->drv_, &status);
                    g_seen.pending = c->pending();
                    {
                        IrqLock lock;
                        g_seen.record_retiring = task_irq_retiring(task_decl_record(0))->head != nullptr;
                    }
                    kos_task_t t = KOS_TASK_NONE;
                    g_seen.create = c->create_drv(&t);
                    g_seen.ran = true;
                    g_core = 1;
                }

                // Inside core one's dispatch, as core zero: main lets go of the line drv declares,
                // never having started drv, and arms its watch on drv.
                static void line_released(void*)
                {
                    g_core = 0;
                    SysGraceDrive* const c = g_case;
                    {
                        IrqLock lock;
                        (void)handle_close(c->main_, c->line_, lock);
                    }
                    g_seen.armed = c->watch_drv();
                    g_seen.pending = c->pending();
                    kos_task_t t = KOS_TASK_NONE;
                    g_seen.create = c->create_drv(&t);
                    g_seen.ran = true;
                    g_core = 1;
                }

                // Inside core one's dispatch, as core zero: main lets go of the line usr declares,
                // arms its watch on usr, and claims the line again.
                static void user_line_released(void*)
                {
                    g_core = 0;
                    SysGraceDrive* const c = g_case;
                    {
                        IrqLock lock;
                        (void)handle_close(c->main_, c->line_, lock);
                    }
                    g_seen.armed = c->watch_named("usr");
                    g_seen.pending = c->pending();
                    uint32_t again = KCAP_INVALID;
                    g_seen.claim = irq_claim(c->main_, LINE, 0, &again);
                    g_seen.ran = true;
                    g_core = 1;
                }

                // drv started, its entry holding the line.
                void drv_running()
                {
                    ASSERT_EQ(create_drv(&drv_), 0);
                    entry_ = entry_of(drv_);
                    ASSERT_NE(entry_, nullptr);
                    running() = main_;
                }

                Thread* entry_ = nullptr;
                kos_task_t drv_ = KOS_TASK_NONE;
                uint32_t line_ = KCAP_INVALID;
                uint32_t main_note_ = KCAP_INVALID;
                uint32_t watch_note_ = KCAP_INVALID;
                uint32_t watch_copy_ = KCAP_INVALID;
            };
        }

        // The red arm: the death alone does not make drv startable while its line's binding
        // retires, and the watch is raised by the reclamation's own pass.
        TEST_F(SysGraceDrive, an_instance_watch_is_raised_by_the_reclamation_after_the_grace_and_not_at_the_death)
        {
            drv_running();
            int64_t const w = watch_drv();
            ASSERT_GE(w, 0) << "a live drv is in the way";
            ASSERT_TRUE(irq_attach(LINE_CARRIER, drv_dies, nullptr));
            g_core = 1;
            kickos_isr_irq(LINE_CARRIER);
            g_core = 0;
            ASSERT_TRUE(g_seen.ran) << "fixture: the handler never ran in core one's dispatch";
            EXPECT_NE(g_seen.state & KOS_TASK_DEAD, 0) << "fixture: drv is not dead";
            EXPECT_TRUE(g_seen.record_retiring) << "fixture: drv's binding is not retiring";
            EXPECT_EQ(g_seen.pending, 0u) << "raised at the death, the grace still open";
            EXPECT_EQ(g_seen.create, -KOS_EAGAIN) << "a create inside the grace";
            EXPECT_EQ(main_->wait_kind, WAIT_NONE) << "the create parked";
            EXPECT_EQ(pending(), 0u) << "raised by core one leaving its dispatch, before any pass";
            dispatch_as(1);
            EXPECT_EQ(pending(), 1u << WATCH_BIT) << "the reclaiming pass did not raise the watch";
            {
                IrqLock lock;
                EXPECT_EQ(task_irq_retiring(task_decl_record(0))->head, nullptr);
            }
            kos_task_t t = KOS_TASK_NONE;
            EXPECT_EQ(create_drv(&t), 0) << "the first create after the event";
            EXPECT_NE(t, KOS_TASK_NONE);
            IrqLock lock;
            EXPECT_EQ(handle_close(main_, static_cast<uint32_t>(w), lock), 0);
        }

        // Nothing watches and nothing waits: the batch still completes.
        TEST_F(SysGraceDrive, the_grace_completes_with_no_watcher)
        {
            drv_running();
            ASSERT_TRUE(irq_attach(LINE_CARRIER, drv_dies, nullptr));
            g_core = 1;
            kickos_isr_irq(LINE_CARRIER);
            g_core = 0;
            ASSERT_TRUE(g_seen.ran);
            ASSERT_TRUE(g_seen.record_retiring) << "fixture: drv's binding is not retiring";
            EXPECT_EQ(g_seen.create, -KOS_EAGAIN);
            dispatch_as(1);
            {
                IrqLock lock;
                EXPECT_EQ(task_irq_retiring(task_decl_record(0))->head, nullptr)
                    << "the batch waited on a watcher";
            }
            kos_task_t t = KOS_TASK_NONE;
            EXPECT_EQ(create_drv(&t), 0);
        }

        // A line drv declares, released by another holder while drv's record is free: the watch
        // waits for that line's reclamation alone.
        TEST_F(SysGraceDrive, a_declared_line_another_holder_released_raises_the_watch_at_its_reclamation)
        {
            ASSERT_EQ(irq_claim(main_, LINE, 0, &line_), 0) << "fixture: the line's last holder";
            ASSERT_TRUE(irq_attach(LINE_CARRIER, line_released, nullptr));
            g_core = 1;
            kickos_isr_irq(LINE_CARRIER);
            g_core = 0;
            ASSERT_TRUE(g_seen.ran);
            EXPECT_GE(g_seen.armed, 0) << "the retiring line was not in drv's way";
            EXPECT_EQ(g_seen.pending, 0u);
            EXPECT_EQ(g_seen.create, -KOS_EAGAIN);
            dispatch_as(1);
            EXPECT_EQ(pending(), 1u << WATCH_BIT) << "the line's reclamation did not raise the watch";
            kos_task_t t = KOS_TASK_NONE;
            EXPECT_EQ(create_drv(&t), 0);
        }

        // A user task's line, which its creator claims and not its create: released by the
        // creator inside core one's dispatch, it retires at the creator's own charge, and the
        // watch on usr still waits for it, raised only once a claim of it can succeed.
        TEST_F(SysGraceDrive, a_user_task_s_line_its_creator_released_holds_the_watch_until_its_reclamation)
        {
            declare(USER.decl, 1);
            ASSERT_EQ(irq_claim(main_, LINE, 0, &line_), 0) << "fixture: the line's last holder";
            ASSERT_TRUE(irq_attach(LINE_CARRIER, user_line_released, nullptr));
            g_core = 1;
            kickos_isr_irq(LINE_CARRIER);
            g_core = 0;
            ASSERT_TRUE(g_seen.ran);
            EXPECT_GE(g_seen.armed, 0) << "answered nothing in the way while its line retires";
            EXPECT_EQ(g_seen.pending, 0u);
            EXPECT_EQ(g_seen.claim, -KOS_EAGAIN) << "fixture: the line does not retire";
            dispatch_as(1);
            EXPECT_EQ(pending(), 1u << WATCH_BIT) << "the line's reclamation did not raise the watch";
            uint32_t again = KCAP_INVALID;
            EXPECT_EQ(irq_claim(main_, LINE, 0, &again), 0) << "raised before the claim can succeed";
        }
    }
}
