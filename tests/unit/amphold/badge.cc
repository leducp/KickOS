// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// THE ENDPOINT BADGE, over the real capability layer and the real IPC paths at a node's posture:
// the mint and what it refuses, and the sender's badge in the receiver's kos_recv_info on every
// path a message reaches a receiver by. A path that dropped the badge would hand the receiver
// KOS_BADGE_NONE, which a bus service refuses and an identity check cannot tell from an
// unbadged client, so every arm names a badge with bits in every byte.

#include <string.h>

#include <kickos/ampwindow.h>
#include <kickos/cap.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

#include "amp_hold_seam.h"
#include "kseam_test.h"
#include "syscall_internal.h"

extern "C" struct arch_context* kickos_ipc_fastpath(uint32_t* args);

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            constexpr uint32_t ME = 0;
            constexpr uint32_t PEER = 1;
            constexpr uint32_t PORT_SERVED = 2;
            constexpr uint32_t PORT_FAR = 3;

            constexpr uint8_t PRIO_SENDER = 20;
            constexpr uint8_t PRIO_RECEIVER = 10;

            constexpr uint32_t BADGE = 0xA5C3E10Fu;
            constexpr uint32_t MSG_LEN = 4;
            constexpr uint32_t RECV_CAP = 16;
            constexpr amp::ReplyTag PEER_TAG = {0x0A0B0C0Du, 0x00001234u};

            uint8_t g_sbuf[RECV_CAP];
            uint8_t g_rbuf[RECV_CAP];

            void amp_reset()
            {
                for (unsigned cls = 0; cls < static_cast<unsigned>(amp::Class::CLASS_MAX); cls++)
                {
                    for (uint32_t to = 0; to < amp::NODE_MAX; to++)
                    {
                        for (uint32_t from = 0; from < amp::NODE_MAX; from++)
                        {
                            amp::Ring& r = amp::ring_for(static_cast<amp::Class>(cls), to, from);
                            r.head.v.store(0u);
                            r.tail.v.store(0u);
                            for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
                            {
                                r.slot[i].len.store(0u);
                                r.slot[i].port.store(amp::PORT_MAX);
                                r.slot[i].tag = amp::ReplyTag{};
                                r.slot[i].badge = 0u;
                            }
                        }
                    }
                }
                amphold::window_cold();
                amp::window_init();
            }

            class IpcBadge : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    amp_reset();
                    memset(g_sbuf, 0x5A, sizeof(g_sbuf));
                    memset(g_rbuf, 0, sizeof(g_rbuf));
                }
            };

            Thread* seat_holder(int slot, uint8_t prio)
            {
                Thread* const t = seat_pool(slot, prio);
                attach_caps(t, KICKOS_SEAM_CAP_WIDTH);
                IrqLock lock;
                sched::reschedule(nullptr, lock);
                return t;
            }

            uint32_t rights_of(Thread* t, uint32_t cap)
            {
                IrqLock lock;
                CapEntry const* const e = cap_lookup(t, cap);
                if (e == nullptr)
                {
                    return 0xFFu;
                }
                return e->rights;
            }

            // A WAIT copy of the endpoint `src` names in s's table, in r's.
            uint32_t give_wait(Thread* s, uint32_t src, Thread* r)
            {
                IrqLock lock;
                CapEntry const* const e = cap_lookup(s, src);
                EXPECT_NE(e, nullptr);
                int const obj = e->obj;
                EXPECT_TRUE(obj_ref_inc(CapType::CAP_ENDPOINT, obj, CAP_WAIT));
                CapEntry const* const ne = cap_install_at(r, KICKOS_CAP_FIRST_DYNAMIC, obj,
                                                          CapType::CAP_ENDPOINT, CAP_WAIT,
                                                          KCAP_BADGE_NONE);
                return (static_cast<uint32_t>(ne->gen) << KCAP_INDEX_BITS)
                       | KICKOS_CAP_FIRST_DYNAMIC;
            }

            // A receive park as endpoint_recv_locked leaves it, on a thread not running.
            void park_receiver(Thread* r, uint32_t ep_cap, kos_reply_recv_opts* opts)
            {
                IrqLock lock;
                int err = 0;
                Endpoint* const e = static_cast<Endpoint*>(
                    cap_resolve_e(r, ep_cap, CapType::CAP_ENDPOINT, CAP_WAIT, &err));
                ASSERT_NE(e, nullptr);
                testfix::seat_blocked(r);
                policy_on_remove(r);
                r->ipc.buf = reinterpret_cast<uintptr_t>(g_rbuf);
                r->ipc.len = RECV_CAP;
                r->ipc.badge_out = reinterpret_cast<uintptr_t>(&opts->info);
                r->wait_queue = &e->recv_waiters;
                r->wait_kind = WAIT_EP_RECV;
                r->wait_obj = e;
                r->wait_result = WAIT_RESULT_POISON;
                e->recv_waiters.push_back(&r->link);
            }

            int32_t receive(uint32_t ep, kos_reply_recv_opts* opts)
            {
                kos_reply_recv_opts_init(opts, ep, 0u, KOS_TIMEOUT_NONE);
                opts->info.badge = 0xDEADDEADu;
                return endpoint_reply_recv(KOS_CAP_NONE, reinterpret_cast<uintptr_t>(g_rbuf),
                                           kos_call_lens_pack(0, RECV_CAP),
                                           reinterpret_cast<uintptr_t>(opts));
            }

            // A sender and a receiver of one endpoint the sender created: `full` its own
            // capability, `badged` a copy carrying BADGE, `wait` the receiver's.
            struct Pair
            {
                Thread* s;
                Thread* r;
                uint32_t full;
                uint32_t badged;
                uint32_t wait;
            };

            void seat_pair(Pair* p)
            {
                p->r = seat_holder(2, PRIO_RECEIVER);
                p->s = seat_holder(1, PRIO_SENDER);
                ASSERT_EQ(sched::current(), p->s);
                ASSERT_EQ(endpoint_create(&p->full), 0);
                ASSERT_EQ(endpoint_badge(p->s, p->full, BADGE, &p->badged), 0);
                p->wait = give_wait(p->s, p->full, p->r);
            }

            // The receiving side, run as the receiver at the sender's park.
            Pair g_pair;
            int32_t g_rc_at_park = 0;
            kos_reply_recv_opts g_opts_at_park;

            void receive_at_park(Thread*)
            {
                g_rc_at_park = receive(g_pair.wait, &g_opts_at_park);
            }

            void receive_and_answer_at_park(Thread*)
            {
                g_rc_at_park = receive(g_pair.wait, &g_opts_at_park);
                if (g_opts_at_park.info.reply_cap != KOS_CAP_NONE)
                {
                    EXPECT_EQ(endpoint_reply(g_opts_at_park.info.reply_cap,
                                             reinterpret_cast<uintptr_t>(g_rbuf), 1u),
                              0);
                }
            }

            kos_reply_recv_opts g_parked_opts;

            void answer_parked_at_park(Thread*)
            {
                EXPECT_NE(g_parked_opts.info.reply_cap, KOS_CAP_NONE);
                EXPECT_EQ(endpoint_reply(g_parked_opts.info.reply_cap,
                                         reinterpret_cast<uintptr_t>(g_rbuf), 1u),
                          0);
            }
        }

        TEST_F(IpcBadge, the_mint_refuses_no_badge_a_badged_source_and_a_source_without_handout)
        {
            Thread* const t = seat_holder(1, PRIO_SENDER);
            uint32_t full = KCAP_INVALID;
            ASSERT_EQ(endpoint_create(&full), 0);

            uint32_t out = 0x1234u;
            EXPECT_EQ(endpoint_badge(t, full, KOS_BADGE_NONE, &out), -KOS_EINVAL)
                << "the unbadged value was minted as a badge";
            EXPECT_EQ(out, KCAP_INVALID);

            uint32_t badged = KCAP_INVALID;
            ASSERT_EQ(endpoint_badge(t, full, BADGE, &badged), 0);
            EXPECT_EQ(rights_of(t, badged), static_cast<uint32_t>(CAP_SIGNAL | CAP_TRANSFER))
                << "the copy carries a right beyond sending and passing it on";
            uint32_t again = KCAP_INVALID;
            EXPECT_EQ(endpoint_badge(t, badged, BADGE + 1u, &again), -KOS_EACCES)
                << "a badged holder minted another client's badge";
            EXPECT_EQ(again, KCAP_INVALID);

            uint32_t narrow = KCAP_INVALID;
            {
                IrqLock lock;
                CapEntry const* const e = cap_lookup(t, full);
                ASSERT_TRUE(obj_ref_inc(CapType::CAP_ENDPOINT, e->obj, CAP_SIGNAL | CAP_TRANSFER));
                ASSERT_EQ(cap_install(t, e->obj, CapType::CAP_ENDPOINT, CAP_SIGNAL | CAP_TRANSFER,
                                      &narrow),
                          0);
            }
            EXPECT_EQ(endpoint_badge(t, narrow, BADGE, &out), -KOS_EACCES)
                << "a holder without HANDOUT minted a badge";

            uint32_t note = KCAP_INVALID;
            ASSERT_EQ(notify_create(t, &note), 0);
            EXPECT_EQ(endpoint_badge(t, note, BADGE, &out), -KOS_EBADF);

            // The control: the unbadged full capability still mints, a second badge.
            EXPECT_EQ(endpoint_badge(t, full, BADGE + 1u, &out), 0);
        }

        TEST_F(IpcBadge, a_send_to_a_parked_receiver_delivers_the_badge)
        {
            ASSERT_NO_FATAL_FAILURE(seat_pair(&g_pair));
            kos_reply_recv_opts opts{};
            opts.info.badge = 0xDEADDEADu;
            park_receiver(g_pair.r, g_pair.wait, &opts);
            ASSERT_EQ(endpoint_send(g_pair.badged, reinterpret_cast<uintptr_t>(g_sbuf), MSG_LEN,
                                    KOS_TIMEOUT_NONE),
                      static_cast<int32_t>(MSG_LEN));
            EXPECT_EQ(opts.info.badge, BADGE);

            // The control on the same pair: the unbadged capability delivers none.
            {
                IrqLock lock;
                sched::reschedule(nullptr, lock);
            }
            ASSERT_EQ(sched::current(), g_pair.s);
            kos_reply_recv_opts plain{};
            plain.info.badge = 0xDEADDEADu;
            park_receiver(g_pair.r, g_pair.wait, &plain);
            ASSERT_EQ(endpoint_send(g_pair.full, reinterpret_cast<uintptr_t>(g_sbuf), MSG_LEN,
                                    KOS_TIMEOUT_NONE),
                      static_cast<int32_t>(MSG_LEN));
            EXPECT_EQ(plain.info.badge, KOS_BADGE_NONE);
        }

        TEST_F(IpcBadge, a_receiver_finding_a_parked_sender_is_handed_its_badge)
        {
            ASSERT_NO_FATAL_FAILURE(seat_pair(&g_pair));
            g_rc_at_park = 0;
            wake_next_park(receive_at_park);
            int32_t const rc = endpoint_send(g_pair.badged, reinterpret_cast<uintptr_t>(g_sbuf),
                                             MSG_LEN, KOS_TIMEOUT_NONE);
            EXPECT_EQ(g_rc_at_park, static_cast<int32_t>(MSG_LEN)) << "the receive found no sender";
            EXPECT_EQ(g_opts_at_park.info.badge, BADGE)
                << "the parked sender's badge did not reach the receiver";
            EXPECT_EQ(g_opts_at_park.info.reply_cap, KOS_CAP_NONE);
            EXPECT_EQ(rc, static_cast<int32_t>(MSG_LEN));
        }

        TEST_F(IpcBadge, a_call_to_a_parked_receiver_delivers_the_badge)
        {
            ASSERT_NO_FATAL_FAILURE(seat_pair(&g_pair));
            g_parked_opts = {};
            g_parked_opts.info.badge = 0xDEADDEADu;
            park_receiver(g_pair.r, g_pair.wait, &g_parked_opts);
            wake_next_park(answer_parked_at_park);
            int32_t const rc = endpoint_call(g_pair.badged, reinterpret_cast<uintptr_t>(g_sbuf),
                                             MSG_LEN, RECV_CAP, KOS_TIMEOUT_NONE);
            EXPECT_EQ(g_parked_opts.info.badge, BADGE);
            EXPECT_EQ(rc, 1);
        }

        TEST_F(IpcBadge, a_receiver_taking_a_parked_call_is_handed_its_badge)
        {
            ASSERT_NO_FATAL_FAILURE(seat_pair(&g_pair));
            g_rc_at_park = 0;
            wake_next_park(receive_and_answer_at_park);
            int32_t const rc = endpoint_call(g_pair.badged, reinterpret_cast<uintptr_t>(g_sbuf),
                                             MSG_LEN, RECV_CAP, KOS_TIMEOUT_NONE);
            EXPECT_EQ(g_rc_at_park, static_cast<int32_t>(MSG_LEN)) << "the receive found no call";
            EXPECT_NE(g_opts_at_park.info.reply_cap, KOS_CAP_NONE);
            EXPECT_EQ(g_opts_at_park.info.badge, BADGE)
                << "the parked caller's badge did not reach the receiver";
            EXPECT_EQ(rc, 1);
        }

        TEST_F(IpcBadge, the_register_fastpath_delivers_the_badge)
        {
            Thread* const c = seat_pool(1, PRIO_RECEIVER);
            attach_caps(c, KICKOS_SEAM_CAP_WIDTH);
            Endpoint* const ep = endpoint();
            ep->recv_holders = 1;
            int const obj = kernel().endpoints.handle_for(kernel().endpoints.index_of(ep));
            kernel().endpoints.refs()[kernel().endpoints.index_of(ep)] = 1;
            uint32_t src = KCAP_INVALID;
            ASSERT_EQ(cap_install(c, obj, CapType::CAP_ENDPOINT, CAP_SIGNAL | CAP_HANDOUT, &src),
                      0);
            uint32_t badged = KCAP_INVALID;
            ASSERT_EQ(endpoint_badge(c, src, BADGE, &badged), 0);

            Thread* const w = spawn(0, PRIO_RECEIVER);
            attach_caps(w, KICKOS_SEAM_CAP_WIDTH);
            uint32_t recv_buf[KOS_CALL_REG_WORDS] = {};
            kos_recv_info info = {0xDEADDEADu, KOS_CAP_NONE};
            {
                IrqLock lock;
                testfix::seat_blocked(w);
                policy_on_remove(w);
                w->wait_queue = &ep->recv_waiters;
                w->wait_kind = WAIT_EP_RECV;
                w->wait_obj = ep;
                w->ipc.buf = reinterpret_cast<uintptr_t>(recv_buf);
                w->ipc.len = sizeof(recv_buf);
                w->ipc.badge_out = reinterpret_cast<uintptr_t>(&info);
                ep->recv_waiters.push_back(&w->link);
                sched::reschedule(nullptr, lock);
            }
            ASSERT_EQ(sched::current(), c);
            uint32_t args[KOS_CALL_REG_WORDS + 3] = {};
            args[0] = KOS_SYS_CALL_REG;
            args[1] = badged;
            args[2] = static_cast<uint32_t>(kos_call_lens_pack(4, KOS_CALL_REG_BYTES));
            uint32_t const taken = ipc_fast_taken_count();
            ASSERT_NE(kickos_ipc_fastpath(args), nullptr) << "the fastpath refused the call";
            EXPECT_EQ(ipc_fast_taken_count(), taken + 1u);
            EXPECT_NE(info.reply_cap, KOS_CAP_NONE);
            EXPECT_EQ(info.badge, BADGE) << "the fastpath dropped the caller's badge";
        }

        namespace
        {
            void far_call_arrives(Thread*)
            {
                amp::Ring& r = amp::ring_for(amp::Class::CALL, ME, PEER);
                uint32_t const head = r.head.v.load();
                amp::Slot& s = r.slot[head % amp::RING_SLOTS];
                s.port.store(PORT_SERVED);
                s.tag = PEER_TAG;
                s.badge = BADGE;
                s.len.store(MSG_LEN);
                for (uint32_t i = 0; i < MSG_LEN; i++)
                {
                    s.payload[i] = static_cast<uint8_t>(0x40u + i);
                }
                r.head.v.store(head + 1u);
                amp::node_service(kickos::IrqLock());
            }
        }

        TEST_F(IpcBadge, a_far_call_lands_with_the_badge_its_slot_carried)
        {
            Thread* const r = seat_holder(1, PRIO_RECEIVER);
            uint32_t ep = KCAP_INVALID;
            {
                IrqLock lock;
                ASSERT_EQ(amp_port_bind_local(r, PORT_SERVED, &ep), 0);
            }
            wake_next_park(far_call_arrives);
            kos_reply_recv_opts opts{};
            ASSERT_EQ(receive(ep, &opts), static_cast<int32_t>(MSG_LEN));
            ASSERT_NE(opts.info.reply_cap, KOS_CAP_NONE);
            EXPECT_EQ(opts.info.badge, BADGE) << "the far record lost the caller's badge";
            EXPECT_EQ(endpoint_reply(opts.info.reply_cap, reinterpret_cast<uintptr_t>(g_rbuf), 0u),
                      0);
        }

        TEST_F(IpcBadge, a_send_through_a_badged_far_endpoint_publishes_the_badge)
        {
            Thread* const c = seat_holder(1, PRIO_SENDER);
            uint32_t far = KCAP_INVALID;
            {
                IrqLock lock;
                ASSERT_EQ(amp_endpoint_mint(c, PEER, PORT_FAR, CAP_SIGNAL | CAP_HANDOUT, &far), 0);
            }
            uint32_t badged = KCAP_INVALID;
            ASSERT_EQ(endpoint_badge(c, far, BADGE, &badged), 0);
            amp::Ring const& out = amp::ring_for(amp::Class::CALL, PEER, ME);
            uint32_t const head = out.head.v.load();
            ASSERT_EQ(endpoint_send(badged, reinterpret_cast<uintptr_t>(g_sbuf), MSG_LEN, 0u),
                      static_cast<int32_t>(MSG_LEN));
            ASSERT_EQ(out.head.v.load(), head + 1u) << "nothing crossed to the peer";
            EXPECT_EQ(out.slot[head % amp::RING_SLOTS].badge, BADGE)
                << "the far publication dropped the sender's badge";

            ASSERT_EQ(endpoint_send(far, reinterpret_cast<uintptr_t>(g_sbuf), MSG_LEN, 0u),
                      static_cast<int32_t>(MSG_LEN));
            EXPECT_EQ(out.slot[(head + 1u) % amp::RING_SLOTS].badge, KOS_BADGE_NONE);
        }
    }
}
