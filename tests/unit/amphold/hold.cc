// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A far call's payload is landed by its RECEIVER, out of the call slot its record holds, in
// the receive that parked it, and a far reply's by its CALLER, out of the reply slot it holds,
// in the call that parked it; the doorbell service copies neither. Every way either thread can
// leave settles what it holds: the landing answers or releases it, and an exit that never lands
// gives it back.
//
// The arm is node 1, the far side, and writes that node's half of every ring by hand. THE
// SERVICE IS DRIVEN FROM A PARK'S WAKER, which is where a doorbell lands on a parked thread;
// what the waker reads of the thread's buffer is the state the masked body left.

#include <string.h>

#include <kickos/ampwindow.h>
#include <kickos/arch/arch.h>
#include <kickos/cap.h>
#include <kickos/endpoint.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

#include "amp_hold_seam.h"
#include "ipc_seam.h"
#include "kseam_test.h"
#include "syscall_internal.h"

// The trap's entry point. Returns the incoming thread's context, or nullptr for a refusal.
extern "C" struct arch_context* kickos_ipc_fastpath(uint32_t* args);

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            constexpr uint32_t ME = 0;
            constexpr uint32_t PEER = 1;
            // The partition's two entries: port 2 is served here, port 3 by the peer.
            constexpr uint32_t PORT_SERVED = 2;

            constexpr uint8_t PRIO_RECEIVER = 10;
            constexpr uint8_t PRIO_ABOVE = 20;
            constexpr uint8_t PRIO_BELOW = 5;

            constexpr uint8_t POISON = 0xEEu;
            constexpr uint8_t CALL_FILL = 0x40u;
            constexpr uint32_t CALL_LEN = 8;
            constexpr uint32_t RECV_CAP = 16;
            constexpr amp::ReplyTag PEER_TAG = {0x0A0B0C0Du, 0x00001234u};

            // Both rings of every pair empty, then this node's rows seated the way boot does.
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
                                memset(r.slot[i].payload, 0, sizeof(r.slot[i].payload));
                            }
                        }
                    }
                }
                amp::window_init();
            }

            amp::Ring& call_ring()
            {
                return amp::ring_for(amp::Class::CALL, ME, PEER);
            }

            // The ring this node answers the peer into, the peer being its consumer.
            amp::Ring& answer_ring()
            {
                return amp::ring_for(amp::Class::REPLY, PEER, ME);
            }

            // The peer publishing one well-formed call on this node's port.
            void peer_calls(uint8_t fill)
            {
                amp::Ring& r = call_ring();
                uint32_t const head = r.head.v.load();
                amp::Slot& s = r.slot[head % amp::RING_SLOTS];
                s.port.store(PORT_SERVED);
                s.tag = PEER_TAG;
                s.len.store(CALL_LEN);
                for (uint32_t i = 0; i < CALL_LEN; i++)
                {
                    s.payload[i] = static_cast<uint8_t>(fill + i);
                }
                r.head.v.store(head + 1u);
            }

            // The doorbell's body, under the mask a doorbell runs with.
            void doorbell()
            {
                IrqLock lock;
                amp::node_service();
            }

            bool answered_empty_at(uint32_t index)
            {
                amp::Slot const& s = answer_ring().slot[index % amp::RING_SLOTS];
                return s.len.load() == 0u and s.port.load() == amp::PORT_REPLY
                       and s.tag.thread == PEER_TAG.thread and s.tag.seq == PEER_TAG.seq;
            }

            // --- one receive, and what the service left behind it ------------------------
            uint8_t g_rbuf[RECV_CAP];
            uint8_t g_rbuf_at_service[RECV_CAP];
            uint32_t g_hold_at_service = 0;
            uint32_t g_tail_before = 0;
            uint32_t g_tail_at_service = 0;
            void (*g_after_service)() = nullptr;

            void poison_rbuf()
            {
                memset(g_rbuf, POISON, sizeof(g_rbuf));
                memset(g_rbuf_at_service, 0, sizeof(g_rbuf_at_service));
            }

            bool rbuf_holds_the_call(uint8_t const* buf)
            {
                for (uint32_t i = 0; i < CALL_LEN; i++)
                {
                    if (buf[i] != static_cast<uint8_t>(CALL_FILL + i))
                    {
                        return false;
                    }
                }
                for (uint32_t i = CALL_LEN; i < RECV_CAP; i++)
                {
                    if (buf[i] != POISON)
                    {
                        return false;
                    }
                }
                return true;
            }

            bool rbuf_untouched(uint8_t const* buf)
            {
                for (uint32_t i = 0; i < RECV_CAP; i++)
                {
                    if (buf[i] != POISON)
                    {
                        return false;
                    }
                }
                return true;
            }

            void call_arrives(Thread* parked)
            {
                peer_calls(CALL_FILL);
                g_tail_before = call_ring().tail.v.load();
                amp::node_service();
                memcpy(g_rbuf_at_service, g_rbuf, sizeof(g_rbuf));
                g_hold_at_service = parked->far_hold;
                g_tail_at_service = call_ring().tail.v.load();
                if (g_after_service != nullptr)
                {
                    g_after_service();
                }
            }

            class AmpHold : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    amp_reset();
                    amphold::g_raises = 0;
                    amphold::g_raise_mask = 0;
                    g_after_service = nullptr;
                    amphold::g_after_snapshot = nullptr;
                    amphold::g_snapshot_hooks = 0;
                    g_ipc_seam_refuse_write = 0;
                    poison_rbuf();
                }

                void TearDown() override
                {
                    g_ipc_seam_refuse_write = 0;
                    amphold::g_after_snapshot = nullptr;
                }
            };

            // A pool thread running, with the served port bound to an endpoint it can wait on.
            Thread* seat_receiver(int slot, uint8_t prio, uint32_t* out_ep)
            {
                Thread* const r = seat_pool(slot, prio);
                attach_caps(r, KICKOS_CAP_CHILD_WIDTH);
                IrqLock lock;
                sched::reschedule();
                EXPECT_EQ(amp_port_bind_local(r, PORT_SERVED, out_ep), 0);
                return r;
            }

            int32_t receive(uint32_t ep, kos_reply_recv_opts* opts, uint32_t flags)
            {
                kos_reply_recv_opts_init(opts, ep, flags, KOS_TIMEOUT_NONE);
                return endpoint_reply_recv(KOS_CAP_NONE, reinterpret_cast<uintptr_t>(g_rbuf),
                                           kos_call_lens_pack(0, RECV_CAP),
                                           reinterpret_cast<uintptr_t>(opts));
            }
        }

        // The whole call side: nothing is copied under the mask, the receiver lands the bytes and
        // is handed the reply capability beside them, and the slot stays the caller's record until
        // that reply, which releases it.
        TEST_F(AmpHold, a_far_call_is_landed_by_its_receiver_and_not_by_the_service)
        {
            uint32_t ep = KCAP_INVALID;
            Thread* const r = seat_receiver(1, PRIO_RECEIVER, &ep);
            ASSERT_EQ(sched::current(), r);
            wake_next_park(call_arrives);

            kos_reply_recv_opts opts{};
            int32_t const rc = receive(ep, &opts, 0);

            EXPECT_TRUE(rbuf_untouched(g_rbuf_at_service))
                << "the masked service copied the payload into the receiver's buffer";
            EXPECT_NE(g_hold_at_service, 0u) << "the service woke the receiver holding nothing";
            EXPECT_EQ(g_tail_at_service, g_tail_before) << "the service released the call slot";
            ASSERT_EQ(rc, static_cast<int32_t>(CALL_LEN));
            EXPECT_TRUE(rbuf_holds_the_call(g_rbuf));
            EXPECT_EQ(r->far_hold, 0u) << "the landing left the hold standing";
            ASSERT_NE(opts.info.reply_cap, KOS_CAP_NONE);
            EXPECT_EQ(call_ring().tail.v.load(), g_tail_before)
                << "the slot left the caller's record before the reply";

            ASSERT_EQ(sched::current(), r);
            uint32_t const answers = answer_ring().head.v.load();
            uint8_t const answer[4] = {1u, 2u, 3u, 4u};
            ASSERT_EQ(endpoint_reply(opts.info.reply_cap, reinterpret_cast<uintptr_t>(answer),
                                     sizeof(answer)),
                      0);
            EXPECT_EQ(answer_ring().head.v.load(), answers + 1u);
            EXPECT_EQ(call_ring().tail.v.load(), g_tail_before + 1u) << "the reply held the slot";
        }

        // An info-less receive can be handed no capability, so the landing itself answers the
        // record, and only after the bytes landed.
        TEST_F(AmpHold, an_infoless_receiver_lands_the_datagram_and_answers_its_record)
        {
            uint32_t ep = KCAP_INVALID;
            seat_receiver(1, PRIO_RECEIVER, &ep);
            wake_next_park(call_arrives);
            uint32_t const answers = answer_ring().head.v.load();

            kos_reply_recv_opts opts{};
            int32_t const rc = receive(ep, &opts, KOS_RECV_NO_INFO);

            EXPECT_TRUE(rbuf_untouched(g_rbuf_at_service));
            EXPECT_EQ(g_tail_at_service, g_tail_before);
            ASSERT_EQ(rc, static_cast<int32_t>(CALL_LEN));
            EXPECT_TRUE(rbuf_holds_the_call(g_rbuf));
            EXPECT_EQ(opts.info.reply_cap, KOS_CAP_NONE);
            ASSERT_EQ(answer_ring().head.v.load(), answers + 1u) << "the far caller is unanswered";
            EXPECT_TRUE(answered_empty_at(answers));
            EXPECT_EQ(call_ring().tail.v.load(), g_tail_before + 1u);
        }

        // The receiver's own buffer refuses the landing: it is told the fault, handed nothing, and
        // the far caller is answered.
        TEST_F(AmpHold, a_landing_the_receivers_buffer_refuses_answers_the_record_and_discloses_nothing)
        {
            uint32_t ep = KCAP_INVALID;
            seat_receiver(1, PRIO_RECEIVER, &ep);
            wake_next_park(call_arrives);
            uint32_t const answers = answer_ring().head.v.load();
            uint32_t const faults = amp::counts(ME).deliver_fault.load();
            g_ipc_seam_refuse_write = reinterpret_cast<uintptr_t>(g_rbuf);

            kos_reply_recv_opts opts{};
            int32_t const rc = receive(ep, &opts, 0);
            g_ipc_seam_refuse_write = 0;

            EXPECT_EQ(g_tail_at_service, g_tail_before);
            EXPECT_EQ(rc, -KOS_EFAULT);
            EXPECT_EQ(opts.info.reply_cap, KOS_CAP_NONE);
            EXPECT_EQ(amp::counts(ME).deliver_fault.load(), faults + 1u);
            ASSERT_EQ(answer_ring().head.v.load(), answers + 1u);
            EXPECT_TRUE(answered_empty_at(answers));
            EXPECT_EQ(call_ring().tail.v.load(), g_tail_before + 1u);
        }

        namespace
        {
            // A receive park as endpoint_recv_locked leaves it, on a thread not running.
            void park_receiver(Thread* r, uint32_t ep_cap, kos_reply_recv_opts* opts)
            {
                IrqLock lock;
                int err = 0;
                Endpoint* const e = static_cast<Endpoint*>(
                    cap_resolve_e(r, ep_cap, CapType::CAP_ENDPOINT, CAP_WAIT, &err));
                ASSERT_NE(e, nullptr);
                r->state = ThreadState::BLOCKED;
                kernel().policy->on_remove(r);
                r->ipc.buf = reinterpret_cast<uintptr_t>(g_rbuf);
                r->ipc.len = RECV_CAP;
                r->ipc.badge_out = reinterpret_cast<uintptr_t>(&opts->info);
                r->wait_queue = &e->recv_waiters;
                r->wait_kind = WAIT_EP_RECV;
                r->wait_obj = e;
                r->wait_result = WAIT_RESULT_POISON;
                e->recv_waiters.push_back(&r->link);
            }

            void slay_exit()
            {
                kickos_thread_slay_exit(nullptr);
            }
        }

        // Slain between the delivery and its landing, the receiver never returns into the receive
        // that would have landed the call: its exit answers the record.
        TEST_F(AmpHold, a_receiver_slain_before_it_lands_answers_its_caller_and_releases_the_slot)
        {
            Thread* const above = seat_pool(0, PRIO_ABOVE);
            uint32_t ep = KCAP_INVALID;
            Thread* const r = seat_receiver(1, PRIO_RECEIVER, &ep);
            ASSERT_EQ(sched::current(), above);
            kos_reply_recv_opts opts{};
            kos_reply_recv_opts_init(&opts, ep, 0, KOS_TIMEOUT_NONE);
            ASSERT_NO_FATAL_FAILURE(park_receiver(r, ep, &opts));

            peer_calls(CALL_FILL);
            uint32_t const tail_before = call_ring().tail.v.load();
            uint32_t const answers = answer_ring().head.v.load();
            doorbell();
            ASSERT_EQ(r->state, ThreadState::READY);
            ASSERT_NE(r->far_hold, 0u);
            ASSERT_EQ(call_ring().tail.v.load(), tail_before);

            {
                IrqLock lock;
                thread_cancel_kind(r, CANCEL_SLAY);
                sched::set_prio(above, PRIO_BELOW);
                sched::reschedule();
            }
            ASSERT_EQ(sched::current(), r);
            ASSERT_EQ(g_redirect_target, r) << "the slay claim did not take the resume";
            run_noreturn(slay_exit);

            EXPECT_TRUE(rbuf_untouched(g_rbuf)) << "a landing ran for a thread that was slain";
            EXPECT_EQ(r->far_hold, 0u);
            ASSERT_EQ(answer_ring().head.v.load(), answers + 1u)
                << "the far caller of a slain receiver is never answered";
            EXPECT_TRUE(answered_empty_at(answers));
            EXPECT_EQ(call_ring().tail.v.load(), tail_before + 1u);
        }

        namespace
        {
            // A depth no ring can hold, standing for the whole strike bound, which resynchronises
            // the call ring under the record the receiver has not landed yet. A whole multiple of
            // RING_SLOTS, so the adopted tail masks back onto the held slot.
            void resync_under_the_hold()
            {
                amp::Ring& r = call_ring();
                r.head.v.store(r.tail.v.load() + 2u * amp::RING_SLOTS);
                for (uint32_t i = 0; i < amp::DEPTH_STRIKES; i++)
                {
                    amp::node_service();
                }
            }

            void resync_and_reuse_call_slot()
            {
                resync_under_the_hold();
                // The reset gave this same masked slot back to the producer.
                peer_calls(0x90u);
            }

            void reset_call_after_snapshot()
            {
                IrqLock lock;
                resync_and_reuse_call_slot();
            }
        }

        // The record dies before the landing: the resynchronisation answered its caller, so the
        // receiver reports the lost message and discloses no capability.
        TEST_F(AmpHold, a_record_a_resynchronisation_freed_before_the_landing_is_neither_disclosed_nor_released)
        {
            uint32_t ep = KCAP_INVALID;
            seat_receiver(1, PRIO_RECEIVER, &ep);
            uint32_t const answers = answer_ring().head.v.load();
            g_after_service = resync_under_the_hold;
            wake_next_park(call_arrives);

            kos_reply_recv_opts opts{};
            int32_t const rc = receive(ep, &opts, 0);

            uint32_t const resynced = call_ring().head.v.load();
            ASSERT_EQ(call_ring().tail.v.load(), resynced) << "the resynchronisation never ran";
            EXPECT_EQ(rc, -KOS_EPIPE);
            EXPECT_TRUE(rbuf_untouched(g_rbuf));
            EXPECT_EQ(opts.info.reply_cap, KOS_CAP_NONE);
            EXPECT_EQ(answer_ring().head.v.load(), answers + 1u)
                << "the caller was answered by the landing as well as by the resynchronisation";
            EXPECT_TRUE(answered_empty_at(answers));
        }

        TEST_F(AmpHold, a_reset_and_reuse_cannot_land_a_later_calls_payload)
        {
            uint32_t ep = KCAP_INVALID;
            seat_receiver(1, PRIO_RECEIVER, &ep);
            g_after_service = resync_and_reuse_call_slot;
            wake_next_park(call_arrives);

            kos_reply_recv_opts opts{};
            int32_t const rc = receive(ep, &opts, 0);

            EXPECT_EQ(rc, -KOS_EPIPE);
            EXPECT_TRUE(rbuf_untouched(g_rbuf));
            EXPECT_EQ(opts.info.reply_cap, KOS_CAP_NONE);
            EXPECT_EQ(call_ring().tail.v.load() + 1u, call_ring().head.v.load())
                << "the old receiver released the later call's slot";
        }

        TEST_F(AmpHold, a_reset_after_call_snapshot_is_refused_before_user_copy)
        {
            uint32_t ep = KCAP_INVALID;
            seat_receiver(1, PRIO_RECEIVER, &ep);
            amphold::g_after_snapshot = reset_call_after_snapshot;
            wake_next_park(call_arrives);

            kos_reply_recv_opts opts{};
            int32_t const rc = receive(ep, &opts, 0);

            EXPECT_EQ(amphold::g_snapshot_hooks, 1u);
            EXPECT_EQ(rc, -KOS_EPIPE);
            EXPECT_TRUE(rbuf_untouched(g_rbuf));
            EXPECT_EQ(opts.info.reply_cap, KOS_CAP_NONE);
            EXPECT_EQ(call_ring().tail.v.load() + 1u, call_ring().head.v.load())
                << "the old receiver released the later call's slot";
        }

        // --- the reply side ------------------------------------------------------------
        namespace
        {
            constexpr uint32_t PORT_FAR = 3; // the partition's second entry, the peer's
            constexpr uint8_t REQUEST_FILL = 0x70u;
            constexpr uint8_t REPLY_FILL = 0x90u;
            constexpr uint32_t REPLY_LEN = 6;

            amp::Ring& reply_ring()
            {
                return amp::ring_for(amp::Class::REPLY, ME, PEER);
            }

            // The ring this node's calls go out on, the peer being its consumer.
            amp::Ring& outbound_ring()
            {
                return amp::ring_for(amp::Class::CALL, PEER, ME);
            }

            void peer_replies(amp::ReplyTag const& tag)
            {
                amp::Ring& r = reply_ring();
                uint32_t const head = r.head.v.load();
                amp::Slot& s = r.slot[head % amp::RING_SLOTS];
                s.port.store(amp::PORT_REPLY);
                s.tag = tag;
                s.len.store(REPLY_LEN);
                for (uint32_t i = 0; i < REPLY_LEN; i++)
                {
                    s.payload[i] = static_cast<uint8_t>(REPLY_FILL + i);
                }
                r.head.v.store(head + 1u);
            }

            // The route the caller's last publication carried, as the peer would hand it back.
            amp::ReplyTag published_tag()
            {
                amp::Ring const& r = outbound_ring();
                return r.slot[(r.head.v.load() - 1u) % amp::RING_SLOTS].tag;
            }

            uint8_t g_cbuf[RECV_CAP];
            uint8_t g_cbuf_at_service[RECV_CAP];
            uint32_t g_reply_tail_before = 0;
            uint32_t g_reply_tail_at_service = 0;
            uint32_t g_credit_at_service = 0;

            constexpr uint32_t PEER_CORE_BIT = 1u << 1; // KICKOS_AMP_NODE_CORE_LIST={0,1}

            void poison_cbuf()
            {
                for (uint32_t i = 0; i < RECV_CAP; i++)
                {
                    g_cbuf[i] = POISON;
                }
                for (uint32_t i = 0; i < CALL_LEN; i++)
                {
                    g_cbuf[i] = static_cast<uint8_t>(REQUEST_FILL + i);
                }
            }

            bool cbuf_holds(uint8_t const* buf, uint8_t fill, uint32_t len)
            {
                for (uint32_t i = 0; i < len; i++)
                {
                    if (buf[i] != static_cast<uint8_t>(fill + i))
                    {
                        return false;
                    }
                }
                return true;
            }

            void reply_arrives(Thread* parked)
            {
                peer_replies(published_tag());
                g_reply_tail_before = reply_ring().tail.v.load();
                amphold::g_raise_mask = 0;
                amp::node_service();
                memcpy(g_cbuf_at_service, g_cbuf, sizeof(g_cbuf));
                g_hold_at_service = parked->far_hold;
                g_reply_tail_at_service = reply_ring().tail.v.load();
                g_credit_at_service = amphold::g_raise_mask & PEER_CORE_BIT;
                if (g_after_service != nullptr)
                {
                    g_after_service();
                }
            }

            Thread* g_caller = nullptr;

            void kill_after_the_hold()
            {
                thread_cancel_kind(g_caller, CANCEL_KILL);
            }

            // The deadline fires while the caller waits, and the reply lands after it.
            void timed_out_then_reply_arrives(Thread* parked)
            {
                amp::ReplyTag const tag = published_tag();
                thread_abort_park(parked, -KOS_ETIMEDOUT);
                peer_replies(tag);
                g_reply_tail_before = reply_ring().tail.v.load();
                amphold::g_raise_mask = 0;
                amp::node_service();
                g_hold_at_service = parked->far_hold;
                g_reply_tail_at_service = reply_ring().tail.v.load();
                g_credit_at_service = amphold::g_raise_mask & PEER_CORE_BIT;
            }

            // A pool thread running, holding a far endpoint on the peer's port.
            Thread* seat_caller(int slot, uint8_t prio, uint32_t* out_far)
            {
                Thread* const c = seat_pool(slot, prio);
                attach_caps(c, KICKOS_CAP_CHILD_WIDTH);
                IrqLock lock;
                sched::reschedule();
                EXPECT_EQ(amp_endpoint_mint(c, PEER, PORT_FAR, out_far), 0);
                return c;
            }

            int32_t call(uint32_t far)
            {
                return endpoint_call(far, reinterpret_cast<uintptr_t>(g_cbuf), CALL_LEN, RECV_CAP,
                                     KOS_TIMEOUT_NONE);
            }

            class AmpReplyHold : public AmpHold
            {
              protected:
                void SetUp() override
                {
                    AmpHold::SetUp();
                    poison_cbuf();
                    g_caller = nullptr;
                }
            };
        }

        // The whole reply side: the service copies nothing and releases nothing, the caller lands
        // the bytes in its own call, and the release that frees the slot returns the credit.
        TEST_F(AmpReplyHold, a_far_reply_is_landed_by_its_caller_and_not_by_the_service)
        {
            uint32_t far = KCAP_INVALID;
            Thread* const c = seat_caller(1, PRIO_RECEIVER, &far);
            ASSERT_EQ(sched::current(), c);
            wake_next_park(reply_arrives);

            int32_t const rc = call(far);

            EXPECT_TRUE(cbuf_holds(g_cbuf_at_service, REQUEST_FILL, CALL_LEN))
                << "the masked service copied the reply into the caller's buffer";
            EXPECT_NE(g_hold_at_service, 0u) << "the service woke the caller holding nothing";
            EXPECT_EQ(g_reply_tail_at_service, g_reply_tail_before)
                << "the service released a slot its caller holds";
            EXPECT_EQ(g_credit_at_service, 0u) << "credit returned before the landing";
            ASSERT_EQ(rc, static_cast<int32_t>(REPLY_LEN));
            EXPECT_TRUE(cbuf_holds(g_cbuf, REPLY_FILL, REPLY_LEN));
            EXPECT_EQ(c->far_hold, 0u);
            EXPECT_EQ(reply_ring().tail.v.load(), g_reply_tail_before + 1u)
                << "the landing never released the slot";
            EXPECT_EQ(amphold::g_raise_mask & PEER_CORE_BIT, PEER_CORE_BIT)
                << "the release that freed the slot returned no credit";
        }

        namespace
        {
            void resync_and_reuse_reply_slot()
            {
                amp::Ring& r = reply_ring();
                r.head.v.store(r.tail.v.load() + 2u * amp::RING_SLOTS);
                for (uint32_t i = 0; i < amp::DEPTH_STRIKES; i++)
                {
                    amp::node_service();
                }
                peer_replies(published_tag());
            }

            void reset_reply_after_snapshot()
            {
                IrqLock lock;
                resync_and_reuse_reply_slot();
            }
        }

        TEST_F(AmpReplyHold, a_reset_and_reuse_cannot_land_a_later_reply_payload)
        {
            uint32_t far = KCAP_INVALID;
            Thread* const c = seat_caller(1, PRIO_RECEIVER, &far);
            g_after_service = resync_and_reuse_reply_slot;
            wake_next_park(reply_arrives);

            int32_t const rc = call(far);

            EXPECT_EQ(rc, -KOS_EPIPE);
            EXPECT_TRUE(cbuf_holds(g_cbuf, REQUEST_FILL, CALL_LEN));
            EXPECT_EQ(c->far_hold, 0u);
            EXPECT_EQ(reply_ring().tail.v.load() + 1u, reply_ring().head.v.load())
                << "the old caller released the later reply's slot";
        }

        TEST_F(AmpReplyHold, a_reset_after_reply_snapshot_is_refused_before_user_copy)
        {
            uint32_t far = KCAP_INVALID;
            Thread* const c = seat_caller(1, PRIO_RECEIVER, &far);
            amphold::g_after_snapshot = reset_reply_after_snapshot;
            wake_next_park(reply_arrives);

            int32_t const rc = call(far);

            EXPECT_EQ(amphold::g_snapshot_hooks, 1u);
            EXPECT_EQ(rc, -KOS_EPIPE);
            EXPECT_TRUE(cbuf_holds(g_cbuf, REQUEST_FILL, CALL_LEN));
            EXPECT_EQ(c->far_hold, 0u);
            EXPECT_EQ(reply_ring().tail.v.load() + 1u, reply_ring().head.v.load())
                << "the old caller released the later reply's slot";
        }

        TEST_F(AmpReplyHold, a_reply_landing_the_callers_buffer_refuses_is_efault_and_still_releases)
        {
            uint32_t far = KCAP_INVALID;
            seat_caller(1, PRIO_RECEIVER, &far);
            wake_next_park(reply_arrives);
            uint32_t const faults = amp::counts(ME).deliver_fault.load();
            g_ipc_seam_refuse_write = reinterpret_cast<uintptr_t>(g_cbuf);

            int32_t const rc = call(far);
            g_ipc_seam_refuse_write = 0;

            EXPECT_EQ(rc, -KOS_EFAULT);
            EXPECT_EQ(amp::counts(ME).deliver_fault.load(), faults + 1u);
            EXPECT_EQ(reply_ring().tail.v.load(), g_reply_tail_before + 1u);
        }

        // A caller whose deadline fired first is no longer the one the tag names, so the reply
        // reaches nobody and the service itself gives the slot back.
        TEST_F(AmpReplyHold, a_reply_arriving_after_its_callers_timeout_is_released_by_the_service)
        {
            uint32_t far = KCAP_INVALID;
            Thread* const c = seat_caller(1, PRIO_RECEIVER, &far);
            wake_next_park(timed_out_then_reply_arrives);
            uint32_t const drops = amp::counts(ME).reply_drop.load();

            int32_t const rc = call(far);

            EXPECT_EQ(rc, -KOS_ETIMEDOUT);
            EXPECT_EQ(g_hold_at_service, 0u) << "a caller no longer waiting was handed the slot";
            EXPECT_EQ(amp::counts(ME).reply_drop.load(), drops + 1u);
            EXPECT_EQ(g_reply_tail_at_service, g_reply_tail_before + 1u)
                << "a reply nobody lands was left holding the ring";
            EXPECT_EQ(g_credit_at_service, PEER_CORE_BIT);
            EXPECT_EQ(c->far_hold, 0u);
            EXPECT_TRUE(cbuf_holds(g_cbuf, REQUEST_FILL, CALL_LEN));
        }

        // A kill after the reply is held takes nothing from the call: the caller still returns
        // through its landing, and dies at its next entry.
        TEST_F(AmpReplyHold, a_caller_killed_after_its_reply_is_held_still_lands_and_releases_it)
        {
            uint32_t far = KCAP_INVALID;
            g_caller = seat_caller(1, PRIO_RECEIVER, &far);
            g_after_service = kill_after_the_hold;
            wake_next_park(reply_arrives);

            int32_t const rc = call(far);

            ASSERT_NE(g_hold_at_service, 0u);
            EXPECT_EQ(rc, static_cast<int32_t>(REPLY_LEN));
            EXPECT_TRUE(cbuf_holds(g_cbuf, REPLY_FILL, REPLY_LEN));
            EXPECT_EQ(reply_ring().tail.v.load(), g_reply_tail_before + 1u);
            EXPECT_EQ(g_caller->cancel_kind, CANCEL_KILL);
        }

        namespace
        {
            // A far call park as endpoint_call's far arm leaves it, on a thread not running: the
            // sequence committed, the reply capacity recorded, parked queue-less on the endpoint.
            amp::ReplyTag park_far_caller(Thread* c, uint32_t far)
            {
                IrqLock lock;
                int err = 0;
                Endpoint* const e = static_cast<Endpoint*>(
                    cap_resolve_e(c, far, CapType::CAP_ENDPOINT, CAP_SIGNAL, &err));
                EXPECT_NE(e, nullptr);
                c->call_seq = static_cast<uint16_t>(c->call_seq + 1u);
                amp::ReplyTag const tag = {kernel().threads.handle_for(kernel().threads.index_of(c)),
                                           amp::reply_seq(c->call_seq)};
                c->ipc.buf = reinterpret_cast<uintptr_t>(g_cbuf);
                c->ipc.len = RECV_CAP;
                c->ipc.badge_out = 0;
                c->call_rx_cap = RECV_CAP;
                c->call_state = CALL_REPLY_WAIT;
                c->state = ThreadState::BLOCKED;
                kernel().policy->on_remove(c);
                c->wait_queue = nullptr;
                c->wait_kind = WAIT_EP_FAR_REPLY;
                c->wait_obj = e;
                c->wait_result = WAIT_RESULT_POISON;
                return tag;
            }

            // A caller holding its reply, READY behind a thread that outranks it.
            Thread* held_behind(Thread** out_above)
            {
                *out_above = seat_pool(0, PRIO_ABOVE);
                uint32_t far = KCAP_INVALID;
                Thread* const c = seat_caller(1, PRIO_RECEIVER, &far);
                EXPECT_EQ(sched::current(), *out_above);
                amp::ReplyTag const tag = park_far_caller(c, far);
                peer_replies(tag);
                g_reply_tail_before = reply_ring().tail.v.load();
                doorbell();
                EXPECT_EQ(c->state, ThreadState::READY);
                EXPECT_NE(c->far_hold, 0u);
                EXPECT_EQ(reply_ring().tail.v.load(), g_reply_tail_before);
                amphold::g_raise_mask = 0;
                return c;
            }

            void exit_now()
            {
                sched::exit_current(0, sched::EXIT_RETURN);
            }
        }

        // Slain between the delivery and its landing, the caller never returns into the call that
        // would have landed the reply: its exit releases the slot.
        TEST_F(AmpReplyHold, a_caller_slain_before_it_lands_releases_the_reply_slot)
        {
            Thread* above = nullptr;
            Thread* const c = held_behind(&above);
            ASSERT_FALSE(HasFailure());
            {
                IrqLock lock;
                thread_cancel_kind(c, CANCEL_SLAY);
                sched::set_prio(above, PRIO_BELOW);
                sched::reschedule();
            }
            ASSERT_EQ(sched::current(), c);
            ASSERT_EQ(g_redirect_target, c) << "the slay claim did not take the resume";
            run_noreturn(slay_exit);

            EXPECT_TRUE(cbuf_holds(g_cbuf, REQUEST_FILL, CALL_LEN))
                << "a landing ran for a thread that was slain";
            EXPECT_EQ(c->far_hold, 0u);
            EXPECT_EQ(reply_ring().tail.v.load(), g_reply_tail_before + 1u)
                << "a slain caller's reply slot is never given back";
            EXPECT_EQ(amphold::g_raise_mask & PEER_CORE_BIT, PEER_CORE_BIT);
        }

        TEST_F(AmpReplyHold, a_caller_exiting_before_it_lands_releases_the_reply_slot)
        {
            Thread* above = nullptr;
            Thread* const c = held_behind(&above);
            ASSERT_FALSE(HasFailure());
            {
                IrqLock lock;
                sched::set_prio(above, PRIO_BELOW);
                sched::reschedule();
            }
            ASSERT_EQ(sched::current(), c);
            run_noreturn(exit_now);

            EXPECT_EQ(c->far_hold, 0u);
            EXPECT_EQ(reply_ring().tail.v.load(), g_reply_tail_before + 1u)
                << "an exiting caller's reply slot is never given back";
            EXPECT_EQ(amphold::g_raise_mask & PEER_CORE_BIT, PEER_CORE_BIT);
        }

        // --- the register fastpath at a node's posture -----------------------------------
        // A far endpoint falls through the fastpath to endpoint_call, whose far arm parks the
        // caller WITH a continuation; the fastpath's own park has none, and parks only a local
        // caller. Both are what lets a reply hold be landed at all.
        namespace
        {
            struct FastStage
            {
                Thread* c;
                Thread* w;
                Endpoint* ep;
                uint32_t cap;
                uint32_t args[KOS_CALL_REG_WORDS + 3];
                uint32_t recv_buf[KOS_CALL_REG_WORDS];
                kos_recv_info info;
            };
            FastStage g_fs;

            // A caller on a local endpoint whose one receiver is parked: the staging the fastpath
            // completes on.
            void stage_fast()
            {
                memset(&g_fs, 0, sizeof(g_fs));
                g_fs.c = seat_pool(1, PRIO_RECEIVER);
                attach_caps(g_fs.c, KICKOS_CAP_CHILD_WIDTH);
                g_fs.ep = endpoint();
                g_fs.ep->recv_holders = 1;
                int const obj =
                    kernel().endpoints.handle_for(kernel().endpoints.index_of(g_fs.ep));
                ASSERT_EQ(cap_install(g_fs.c, obj, CapType::CAP_ENDPOINT, CAP_SIGNAL, &g_fs.cap),
                          0);
                g_fs.w = spawn(0, PRIO_RECEIVER);
                attach_caps(g_fs.w, KICKOS_CAP_CHILD_WIDTH);
                {
                    IrqLock lock;
                    g_fs.w->state = ThreadState::BLOCKED;
                    kernel().policy->on_remove(g_fs.w);
                    g_fs.w->wait_queue = &g_fs.ep->recv_waiters;
                    g_fs.w->wait_kind = WAIT_EP_RECV;
                    g_fs.w->wait_obj = g_fs.ep;
                    g_fs.w->ipc.buf = reinterpret_cast<uintptr_t>(g_fs.recv_buf);
                    g_fs.w->ipc.len = sizeof(g_fs.recv_buf);
                    g_fs.w->ipc.badge_out = reinterpret_cast<uintptr_t>(&g_fs.info);
                    g_fs.ep->recv_waiters.push_back(&g_fs.w->link);
                    sched::reschedule();
                }
                ASSERT_EQ(sched::current(), g_fs.c);
                g_fs.args[0] = KOS_SYS_CALL_REG;
                g_fs.args[1] = g_fs.cap;
                g_fs.args[2] = static_cast<uint32_t>(kos_call_lens_pack(4, KOS_CALL_REG_BYTES));
            }
        }

        // THE CONTROL AND THE CLAIM ON ONE STAGING: the local call completes, and the same
        // endpoint named far refuses with nothing touched. A far endpoint holds no local receiver
        // in any state the kernel presents, so the receiver here is what isolates the far clause
        // from the dead-endpoint one behind it.
        TEST_F(AmpReplyHold, a_far_endpoint_falls_through_the_fastpath_and_a_local_one_parks)
        {
            ASSERT_NO_FATAL_FAILURE(stage_fast());
            g_fs.ep->far_node = static_cast<uint8_t>(PEER + 1u);
            uint32_t const out_head = outbound_ring().head.v.load();
            uint32_t const taken = ipc_fast_taken_count();

            EXPECT_EQ(kickos_ipc_fastpath(g_fs.args), nullptr);
            EXPECT_EQ(ipc_fast_taken_count(), taken);
            EXPECT_EQ(g_fs.c->state, ThreadState::RUNNING);
            EXPECT_EQ(g_fs.c->call_frame_parked, 0u);
            EXPECT_EQ(g_fs.w->state, ThreadState::BLOCKED);
            EXPECT_EQ(outbound_ring().head.v.load(), out_head);

            g_fs.ep->far_node = 0u;
            EXPECT_NE(kickos_ipc_fastpath(g_fs.args), nullptr) << "the control does not complete";
            EXPECT_EQ(g_fs.c->call_frame_parked, 1u);
            EXPECT_EQ(g_fs.c->wait_kind, WAIT_EP_REPLY);
        }

        // A reply whose tag names a caller the fastpath parked is refused: that caller resumes
        // through its saved frame with no call to land a hold in, so it must never be handed one.
        TEST_F(AmpReplyHold, a_far_reply_naming_a_fastpath_caller_is_dropped_and_released)
        {
            ASSERT_NO_FATAL_FAILURE(stage_fast());
            ASSERT_NE(kickos_ipc_fastpath(g_fs.args), nullptr);
            ASSERT_EQ(g_fs.c->call_frame_parked, 1u);
            Thread* const c = g_fs.c;
            amp::ReplyTag const tag = {kernel().threads.handle_for(kernel().threads.index_of(c)),
                                       amp::reply_seq(c->call_seq)};
            uint32_t const drops = amp::counts(ME).reply_drop.load();
            peer_replies(tag);
            uint32_t const tail_before = reply_ring().tail.v.load();
            doorbell();

            EXPECT_EQ(c->far_hold, 0u) << "a caller with no continuation was handed a hold";
            EXPECT_EQ(c->state, ThreadState::BLOCKED);
            EXPECT_EQ(c->call_state, CALL_REPLY_WAIT);
            EXPECT_EQ(amp::counts(ME).reply_drop.load(), drops + 1u);
            EXPECT_EQ(reply_ring().tail.v.load(), tail_before + 1u);
        }
    }
}
