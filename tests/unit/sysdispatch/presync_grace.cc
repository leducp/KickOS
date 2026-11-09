// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A packaged driver's create at two kernel cores over the real dispatch, its declared line still
// retiring from its last holder while core one's dispatch holds the grace period open: the create
// staged its image's data at its plan and parks for the grace period holding no stage, so a
// second creator meanwhile is answered the retry, not the busy of a live instance, and once the
// grace period is over the create starts the driver. Single-threaded: core one's dispatch runs
// the arm as core zero from inside its handler.

#include <kickos/irq.h>
#include <kickos/notify.h>
#include <kickos/sync.h>

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

            struct Seen
            {
                bool ran = false;
                bool parked = false;
                arch_phys_addr_t staged = 0;
                ThreadLink stager = 0;
                int32_t second = 1;
                int32_t first = 1;
            };

            class SysPresyncGrace;
            SysPresyncGrace* g_case = nullptr;
            Seen g_seen;

            class SysPresyncGrace : public SysFixture
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
                    editor_ = member(2, MAIN_PRIO, task_);
                    cap_seat_authority(editor_, AUTH_MEMORY | AUTH_TASKS | AUTH_IRQ);
                    detach_ready(editor_);
                    seat_blocked(editor_);
                    main_->affinity = 1u;
                    editor_->affinity = 1u;
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

                // A create of drv by `t` from its parameters on `page`, handing over `note`.
                int32_t create_drv(Thread* t, size_t page, size_t out_page, uint32_t note,
                                   uint32_t timeout_us, kos_task_t* out)
                {
                    kos_task_params* const p = task_params(page, 0, 0, 0);
                    *host<kos_cap_grant>(uva(page, CAPS_AT)) = {
                        note, KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER};
                    p->caps = reinterpret_cast<kos_cap_grant const*>(uva(page, CAPS_AT));
                    p->cap_count = 1;
                    uint32_t* const o = host<uint32_t>(uva(out_page));
                    *o = KOS_TASK_NONE;
                    int32_t const rc = err(call_as(t, KOS_SYS_TASK_CREATE, program_for(t),
                                                   uva(page, TASK_PARAMS_AT), uva(out_page),
                                                   timeout_us));
                    *out = static_cast<kos_task_t>(*o);
                    return rc;
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

                static PresyncRecord& record_of(Thread const* t)
                {
                    return kernel().presync()[kernel().threads.index_of(t)];
                }

                // At the create's park: what it holds, and a second creator's answer.
                static void at_the_park(Thread* parked)
                {
                    SysPresyncGrace* const c = g_case;
                    g_seen.parked = parked == c->main_ and parked->wait_kind == WAIT_RETIRE;
                    g_seen.staged = record_of(parked).staged;
                    g_seen.stager = *task_image_stager(task_decl_record(0));
                    policy_on_ready(c->editor_);
                    c->editor_->state.to<ThreadState::RUNNING>();
                    kos_task_t t = KOS_TASK_NONE;
                    g_seen.second = c->create_drv(c->editor_, EDIT_PARAMS, EDIT_OUT,
                                                  c->editor_note_, 0u, &t);
                    detach_ready(c->editor_);
                    seat_blocked(c->editor_);
                    running() = c->main_;
                    IrqLock lock;
                    thread_abort_park(parked, -KOS_ETIMEDOUT, lock);
                }

                // Inside core one's dispatch, as core zero: the line's last holder lets it go,
                // and the create meets it retiring.
                static void release_and_create(void*)
                {
                    g_core = 0;
                    SysPresyncGrace* const c = g_case;
                    {
                        IrqLock lock;
                        (void)handle_close(c->main_, c->line_, lock);
                    }
                    wake_next_park(at_the_park);
                    kos_task_t t = KOS_TASK_NONE;
                    g_seen.first = c->create_drv(c->main_, MAIN_PARAMS, MAIN_OUT, c->main_note_,
                                                 1000u, &t);
                    g_seen.ran = true;
                    g_core = 1;
                }

                Thread* editor_ = nullptr;
                uint32_t line_ = KCAP_INVALID;
                uint32_t main_note_ = KCAP_INVALID;
                uint32_t editor_note_ = KCAP_INVALID;
            };
        }

        TEST_F(SysPresyncGrace, a_create_parked_for_its_line_s_grace_period_holds_no_stage)
        {
            main_note_ = note_of(main_);
            editor_note_ = note_of(editor_);
            ASSERT_NE(main_note_, KCAP_INVALID);
            ASSERT_NE(editor_note_, KCAP_INVALID);
            ASSERT_EQ(irq_claim(main_, LINE, 0, &line_), 0) << "fixture: the line's last holder";
            ASSERT_TRUE(irq_attach(LINE_CARRIER, release_and_create, nullptr));
            g_core = 1;
            kickos_isr_irq(LINE_CARRIER);
            g_core = 0;
            ASSERT_TRUE(g_seen.ran) << "fixture: the handler never ran in core one's dispatch";
            ASSERT_TRUE(g_seen.parked) << "fixture: the create never parked on the grace period";
            EXPECT_EQ(g_seen.first, -KOS_ETIMEDOUT);
            EXPECT_EQ(g_seen.staged, 0u) << "the create parked holding its stage";
            EXPECT_EQ(g_seen.stager, 0u) << "the create parked holding drv's stager mark";
            EXPECT_EQ(g_seen.second, -KOS_EAGAIN)
                << "a second creator was refused as if drv were live";
            dispatch_as(1);
            kos_task_t t = KOS_TASK_NONE;
            EXPECT_EQ(create_drv(main_, MAIN_PARAMS, MAIN_OUT, main_note_, 1000u, &t), 0)
                << "the grace period over, the create did not start drv";
            EXPECT_NE(t, KOS_TASK_NONE);
        }
    }
}
