// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A capability of each kind, kept while the object it names goes and its slot is taken again at a
// generation whose low 16 bits are the ones the capability's handle carries. The handle alone names
// the new occupant; the whole generation the entry recorded at its install refuses it on the
// resolve, on the copy, on the delegation and on the close.

#include <kickos/cap.h>
#include <kickos/domain.h>
#include <kickos/image.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/notify.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

#include <vector>

#include "sys_fixture.h"
#include "syscall_internal.h"

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
            static_assert(static_cast<int>(CapType::CAP_KIND_MAX) == 8,
                          "a new capability kind needs its stale-entry case here");

            constexpr uint32_t WRAP = 1u << 16;
            uint8_t g_reply_buf[16];

            // Every other free slot of `pool` taken, the slot `handle` names freed, then taken and
            // freed until its generation has come round to the handle's 16 bits. The slot is left
            // free, so the pool's next alloc takes it; the other slots are returned.
            template <typename Pool>
            std::vector<int> wrap_slot(Pool& pool, int handle)
            {
                std::vector<int> others;
                int const slot = pool.live_index(handle);
                while (true)
                {
                    int const i = pool.alloc(nullptr);
                    if (i < 0)
                    {
                        break;
                    }
                    others.push_back(i);
                }
                pool.free(handle);
                for (uint32_t i = 1; i < WRAP; i++)
                {
                    int const at = pool.alloc(nullptr);
                    if (at != slot)
                    {
                        ADD_FAILURE() << "fixture: the pool handed out another slot";
                        return others;
                    }
                    pool.free(pool.handle_for(at));
                }
                return others;
            }

            template <typename Pool>
            void release(Pool& pool, std::vector<int> const& others)
            {
                for (int const i : others)
                {
                    pool.free(pool.handle_for(i));
                }
            }

            class StaleKind : public SysFixture
            {
              protected:
                // A capability in main's table naming `handle`, with the reference it holds.
                uint32_t install(CapType type, int handle, uint8_t rights)
                {
                    IrqLock lock;
                    uint32_t cap = KCAP_INVALID;
                    EXPECT_TRUE(obj_ref_inc(type, handle, rights));
                    EXPECT_EQ(cap_install(main_, handle, type, rights, &cap), 0);
                    return cap;
                }

                // The kept capability resolves to nothing, as a capability whose object is gone.
                void refused(uint32_t cap, CapType type)
                {
                    IrqLock lock;
                    int e = 0;
                    EXPECT_EQ(cap_resolve_e(main_, cap, type, 0, &e), nullptr)
                        << "the kept capability reached the slot's new occupant";
                    EXPECT_EQ(e, KOS_EBADF);
                    CapEntry const* const entry = cap_lookup(main_, cap);
                    ASSERT_NE(entry, nullptr);
                    EXPECT_FALSE(cap_obj_live(*entry));
                    EXPECT_EQ(cap_obj(*entry), -1);
                }

                // A thread main spawns with the kept capability delegated to it is refused.
                void delegation_refused(uint32_t cap)
                {
                    uintptr_t const p = spawn_params(MAIN_PARAMS, 0, 0, 0);
                    *host<kos_cap_grant>(uva(MAIN_PARAMS, CAPS_AT)) = {cap, KOS_CAP_TRANSFER};
                    params(p)->caps = reinterpret_cast<kos_cap_grant const*>(uva(MAIN_PARAMS, CAPS_AT));
                    params(p)->cap_count = 1;
                    kos_thread_t t = KOS_THREAD_NONE;
                    EXPECT_EQ(spawn(main_, p, MAIN_OUT, &t), -KOS_EBADF)
                        << "the kept capability was delegated";
                    EXPECT_EQ(t, KOS_THREAD_NONE);
                }

                // Closing the kept capability empties its entry and leaves the new occupant's
                // references alone.
                void closed(uint32_t cap, uint8_t const& refs)
                {
                    uint8_t const before = refs;
                    EXPECT_EQ(close(main_, cap), 0);
                    EXPECT_EQ(refs, before) << "closing the kept capability dropped a reference of the new occupant";
                    IrqLock lock;
                    EXPECT_EQ(cap_lookup(main_, cap), nullptr);
                }

                // `t` parked in a call, as endpoint_call leaves its caller once a receiver took it.
                static void park_in_call(Thread* t)
                {
                    t->ipc.buf = reinterpret_cast<uintptr_t>(g_reply_buf);
                    t->call_rx_cap = sizeof(g_reply_buf);
                    t->call_state = CALL_REPLY_WAIT;
                    t->wait_kind = WAIT_EP_REPLY;
                    t->wait_result = WAIT_RESULT_POISON;
                }
            };
        }

        TEST_F(StaleKind, an_endpoint)
        {
            Kernel& k = kernel();
            Endpoint* const ep = endpoint();
            int const slot = k.endpoints.index_of(ep);
            int const handle = k.endpoints.handle_for(slot);
            uint32_t const cap = install(CapType::CAP_ENDPOINT, handle, CAP_SIGNAL | CAP_TRANSFER);
            k.endpoints.refs()[slot] = 0;
            std::vector<int> const others = wrap_slot(k.endpoints, handle);
            ASSERT_EQ(endpoint(), ep);
            k.endpoints.refs()[slot] = 1;
            release(k.endpoints, others);
            ASSERT_EQ(k.endpoints.resolve(handle), ep) << "fixture: the kept handle names the new occupant";

            refused(cap, CapType::CAP_ENDPOINT);
            delegation_refused(cap);
            closed(cap, k.endpoints.refs()[slot]);
        }

        TEST_F(StaleKind, a_notification)
        {
            Kernel& k = kernel();
            uint32_t cap = KCAP_INVALID;
            ASSERT_EQ(notify_create(main_, &cap), 0);
            int handle = 0;
            {
                IrqLock lock;
                handle = cap_lookup(main_, cap)->obj;
            }
            int const slot = k.notifies.live_index(handle);
            ASSERT_GE(slot, 0);
            k.notifies.refs()[slot] = 0;
            std::vector<int> const others = wrap_slot(k.notifies, handle);
            ASSERT_EQ(k.notifies.alloc(nullptr), slot);
            Notification* const n = k.notifies.at(slot);
            n->pending = 0;
            n->bound = nullptr;
            n->accept = 0;
            n->signallers = NOTIFY_SIGNALLER_NONE;
            k.notifies.refs()[slot] = 1;
            release(k.notifies, others);
            ASSERT_EQ(k.notifies.resolve(handle), n) << "fixture: the kept handle names the new occupant";

            refused(cap, CapType::CAP_NOTIFY);
            EXPECT_EQ(notify_signal(main_, cap), -KOS_EBADF);
            EXPECT_EQ(n->pending, 0u) << "a signal through the kept capability reached the new occupant";
            uint32_t copy = 0;
            EXPECT_EQ(notify_badge(main_, cap, 3u, &copy), -KOS_EBADF) << "a badged copy was minted";
            EXPECT_EQ(copy, KCAP_INVALID);
            delegation_refused(cap);
            closed(cap, k.notifies.refs()[slot]);
        }

        TEST_F(StaleKind, an_interrupt_binding)
        {
            Kernel& k = kernel();
            int const slot = k.irq_bindings.alloc(nullptr);
            ASSERT_GE(slot, 0);
            *k.irq_bindings.at(slot) = IrqBinding{};
            int const handle = k.irq_bindings.handle_for(slot);
            uint32_t const cap = install(CapType::CAP_IRQ, handle, CAP_WAIT | CAP_TRANSFER);
            k.irq_bindings.refs()[slot] = 0;
            std::vector<int> const others = wrap_slot(k.irq_bindings, handle);
            ASSERT_EQ(k.irq_bindings.alloc(nullptr), slot);
            *k.irq_bindings.at(slot) = IrqBinding{};
            k.irq_bindings.refs()[slot] = 1;
            release(k.irq_bindings, others);
            ASSERT_EQ(k.irq_bindings.resolve(handle), k.irq_bindings.at(slot))
                << "fixture: the kept handle names the new occupant";

            refused(cap, CapType::CAP_IRQ);
            delegation_refused(cap);
            closed(cap, k.irq_bindings.refs()[slot]);
        }

        TEST_F(StaleKind, a_frame_run)
        {
            Kernel& k = kernel();
            RunCaps const run = mint(1);
            ASSERT_NE(run.obj, FRAME_RUN_NONE);
            int const slot = k.frame_runs.live_index(run.obj);
            ASSERT_GE(slot, 0);
            k.frame_runs.refs()[slot] = 0;
            std::vector<int> const others = wrap_slot(k.frame_runs, run.obj);
            ASSERT_EQ(k.frame_runs.alloc(nullptr), slot);
            FrameRun* const f = k.frame_runs.at(slot);
            *f = FrameRun{};
            f->pages = 1;
            k.frame_runs.refs()[slot] = 1;
            release(k.frame_runs, others);
            ASSERT_EQ(k.frame_runs.resolve(run.obj), f) << "fixture: the kept handle names the new occupant";

            refused(run.fcap, CapType::CAP_FRAME);
            delegation_refused(run.fcap);
            closed(run.fcap, k.frame_runs.refs()[slot]);
        }

        TEST_F(StaleKind, an_address_space)
        {
            kos_task_t const first = new_task(main_, 0, 0, 0);
            ASSERT_NE(first, KOS_TASK_NONE);
            Task* t = nullptr;
            {
                IrqLock lock;
                t = task_resolve(first);
            }
            Domain* const d = task_domain(t);
            uint32_t const cap = space_cap(main_, d);
            ASSERT_NE(cap, KCAP_INVALID);
            uint32_t installed = 0;
            int handle = 0;
            {
                IrqLock lock;
                CapEntry const* const e = cap_lookup(main_, cap);
                installed = e->obj_gen;
                handle = e->obj;
            }
            Thread* const entry = entry_of(first);
            ASSERT_NE(entry, nullptr);
            ASSERT_EQ(call_as(main_, KOS_SYS_TASK_SLAY, first), 0u);
            exit_as(entry);
            {
                IrqLock lock;
                task_drop_hold(t, lock);
            }
            ASSERT_EQ(d->refcount, 0u) << "fixture: the domain slot is still held";
            // The next claim advances the generation by one, onto the handle's 16 bits.
            d->generation = installed + WRAP - 1u;
            kos_task_t const next = new_task(main_, 0, 0, 0);
            ASSERT_NE(next, KOS_TASK_NONE);
            {
                IrqLock lock;
                ASSERT_EQ(task_domain(task_resolve(next)), d) << "fixture: the new task took another slot";
            }
            ASSERT_EQ(domain_resolve(handle), d) << "fixture: the kept handle names the new occupant";

            refused(cap, CapType::CAP_ASPACE);
            delegation_refused(cap);
            uint16_t const holds = d->refcount;
            EXPECT_EQ(close(main_, cap), 0);
            EXPECT_EQ(d->refcount, holds);
        }

        TEST_F(StaleKind, a_reply)
        {
            Thread* const caller = seat_pool(2, MAIN_PRIO - 1);
            int const slot = kernel().threads.index_of(caller);
            uint32_t cap = KCAP_INVALID;
            {
                IrqLock lock;
                seat_blocked(caller);
                policy_on_remove(caller);
                park_in_call(caller);
                ASSERT_EQ(cap_install_reply(main_, caller, &cap), 0);
            }
            // The call ends without a reply, and the caller's slot is reclaimed until the handle's
            // 16 bits come round; its new occupant is parked in a call at the same sequence.
            for (uint32_t i = 0; i < WRAP; i++)
            {
                kernel().threads.gen_step(slot);
            }
            {
                IrqLock lock;
                park_in_call(caller);
                CapEntry const* const e = cap_lookup(main_, cap);
                ASSERT_NE(e, nullptr);
                ASSERT_EQ(cap_reply_thread(cap_reply_handle(*e), caller->call_seq, KCAP_REPLY_SEQ_MASK),
                          caller)
                    << "fixture: the handle and the sequence name the new occupant";
                EXPECT_FALSE(cap_obj_live(*e));
                EXPECT_EQ(cap_reply_caller(*e), nullptr) << "the kept reply named the new occupant";
            }
            EXPECT_EQ(close(main_, cap), 0);
            EXPECT_EQ(caller->state, ThreadState::BLOCKED) << "closing the kept reply woke the new occupant";
            EXPECT_EQ(caller->wait_result, WAIT_RESULT_POISON);
        }

        // A build-time program is a static image record, which no slot reuse can take: its
        // capability stays live for as long as the entry does.
        TEST_F(StaleKind, a_program)
        {
            uint32_t const cap = program_for(main_);
            ASSERT_NE(cap, KCAP_INVALID);
            IrqLock lock;
            CapEntry const* const e = cap_lookup(main_, cap);
            ASSERT_NE(e, nullptr);
            EXPECT_TRUE(cap_obj_live(*e));
            int err = 0;
            EXPECT_EQ(cap_resolve_e(main_, cap, CapType::CAP_PROGRAM, 0, &err), image_program(e->obj));
            EXPECT_NE(image_program(e->obj), nullptr);
        }
    }
}
