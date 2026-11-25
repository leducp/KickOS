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

#include "../common/seam_pools.h"

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
            // The partition's entries: port 2 is served here, port 3 by the peer.
            constexpr uint32_t PORT_SERVED = 2;
            constexpr amp::Lane SERVED = amp::lane_of(amp::Class::CALL, ME, PORT_SERVED);
            // Inside the mint width, minted by neither node.
            constexpr uint32_t PORT_UNMINTED = 7u;

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
                            for (uint32_t lane = 0; lane < amp::LANE_MAX; lane++)
                            {
                                r.head.v[lane].store(0u);
                                r.tail.v[lane].store(0u);
                            }
                            for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
                            {
                                r.slot[i].len.store(0u);
                                r.slot[i].port.store(amp::PORT_MAX);
                                r.slot[i].tag = amp::ReplyTag{};
                                r.slot[i].badge = 0u;
                                r.slot[i].withdrawn.store(0u);
                                memset(r.slot[i].payload, 0, sizeof(r.slot[i].payload));
                            }
                        }
                    }
                }
                amphold::window_cold();
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
                uint32_t const head = r.head.v[SERVED.index].load();
                amp::Slot& s = r.slot[SERVED.base + head % SERVED.slots];
                s.port.store(PORT_SERVED);
                s.tag = PEER_TAG;
                s.len.store(CALL_LEN);
                for (uint32_t i = 0; i < CALL_LEN; i++)
                {
                    s.payload[i] = static_cast<uint8_t>(fill + i);
                }
                r.head.v[SERVED.index].store(head + 1u);
            }

            // The peer publishing one send, which waits for no reply, on this node's port.
            void peer_sends(uint8_t fill)
            {
                peer_calls(fill);
                amp::Ring& r = call_ring();
                uint32_t const at = r.head.v[SERVED.index].load() - 1u;
                r.slot[SERVED.base + at % SERVED.slots].tag = amp::REPLY_TAG_NONE;
            }

            // The doorbell's body.
            void doorbell()
            {
                kickos_amp_node_service();
            }

            bool answered_with_at(uint32_t index, uint32_t word)
            {
                amp::Slot const& s = answer_ring().slot[index % amp::RING_SLOTS];
                return s.len.load() == 0u and s.port.load() == amp::PORT_REPLY
                       and s.badge.load() == word and s.tag.thread == PEER_TAG.thread
                       and s.tag.seq == PEER_TAG.seq;
            }

            bool answered_empty_at(uint32_t index)
            {
                return answered_with_at(index, KOS_BADGE_NONE);
            }

            bool refused_at(uint32_t index)
            {
                return answered_with_at(index, amp::REPLY_REFUSED);
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
                g_tail_before = call_ring().tail.v[SERVED.index].load();
                amp::node_service(kickos::IrqLock());
                memcpy(g_rbuf_at_service, g_rbuf, sizeof(g_rbuf));
                g_hold_at_service = parked->far_hold;
                g_tail_at_service = call_ring().tail.v[SERVED.index].load();
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
                attach_caps(r, KICKOS_SEAM_CAP_WIDTH);
                IrqLock lock;
                sched::reschedule(nullptr, lock);
                EXPECT_EQ(amp_port_bind_local(r, PORT_SERVED, out_ep), 0);
                return r;
            }

            int32_t receive(uint32_t ep, kos_reply_recv_opts* opts, uint32_t flags)
            {
                kos_reply_recv_opts_init(opts, ep, flags, KOS_NEVER);
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
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), g_tail_before)
                << "the slot left the caller's record before the reply";

            ASSERT_EQ(sched::current(), r);
            uint32_t const answers = answer_ring().head.v[0].load();
            uint8_t const answer[4] = {1u, 2u, 3u, 4u};
            ASSERT_EQ(endpoint_reply(opts.info.reply_cap, reinterpret_cast<uintptr_t>(answer),
                                     sizeof(answer)),
                      0);
            EXPECT_EQ(answer_ring().head.v[0].load(), answers + 1u);
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), g_tail_before + 1u)
                << "the reply held the slot";

            EXPECT_EQ(endpoint_reply(opts.info.reply_cap, reinterpret_cast<uintptr_t>(answer),
                                     sizeof(answer)),
                      -KOS_EBADF)
                << "the reply capability survived the reply it answered";
            EXPECT_EQ(answer_ring().head.v[0].load(), answers + 1u)
                << "a second reply answered the far caller again";
        }

        // An info-less receive can be handed no capability, so the landing itself answers the
        // record, and only after the bytes landed.
        TEST_F(AmpHold, an_infoless_receiver_lands_the_datagram_and_answers_its_record)
        {
            uint32_t ep = KCAP_INVALID;
            seat_receiver(1, PRIO_RECEIVER, &ep);
            wake_next_park(call_arrives);
            uint32_t const answers = answer_ring().head.v[0].load();

            kos_reply_recv_opts opts{};
            int32_t const rc = receive(ep, &opts, KOS_RECV_NO_INFO);

            EXPECT_TRUE(rbuf_untouched(g_rbuf_at_service));
            EXPECT_EQ(g_tail_at_service, g_tail_before);
            ASSERT_EQ(rc, static_cast<int32_t>(CALL_LEN));
            EXPECT_TRUE(rbuf_holds_the_call(g_rbuf));
            EXPECT_EQ(opts.info.reply_cap, KOS_CAP_NONE);
            ASSERT_EQ(answer_ring().head.v[0].load(), answers + 1u)
                << "the far caller is unanswered";
            EXPECT_TRUE(answered_empty_at(answers));
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), g_tail_before + 1u);
        }

        // The receiver's own buffer refuses the landing: it is told the fault, handed nothing, and
        // the far caller is answered.
        TEST_F(AmpHold, a_landing_the_receivers_buffer_refuses_answers_the_record_and_discloses_nothing)
        {
            uint32_t ep = KCAP_INVALID;
            seat_receiver(1, PRIO_RECEIVER, &ep);
            wake_next_park(call_arrives);
            uint32_t const answers = answer_ring().head.v[0].load();
            uint32_t const faults = amp::counts(ME).deliver_fault.load();
            g_ipc_seam_refuse_write = reinterpret_cast<uintptr_t>(g_rbuf);

            kos_reply_recv_opts opts{};
            int32_t const rc = receive(ep, &opts, 0);
            g_ipc_seam_refuse_write = 0;

            EXPECT_EQ(g_tail_at_service, g_tail_before);
            EXPECT_EQ(rc, -KOS_EFAULT);
            EXPECT_EQ(opts.info.reply_cap, KOS_CAP_NONE);
            EXPECT_EQ(amp::counts(ME).deliver_fault.load(), faults + 1u);
            ASSERT_EQ(answer_ring().head.v[0].load(), answers + 1u);
            EXPECT_TRUE(answered_empty_at(answers));
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), g_tail_before + 1u);
        }

        namespace
        {
            bool g_receiver_parked = false;

        void refuse_the_park(Thread* parked)
        {
            g_receiver_parked = true;
            IrqLock lock;
            thread_abort_park(parked, -KOS_ETIMEDOUT, lock);
        }

        bool rbuf_holds(uint8_t fill);

        void send_arrives(Thread*)
        {
            peer_sends(0x70u);
            amp::node_service(kickos::IrqLock());
        }

        // The receiver takes one send parked, as a server's first receive does.
        void received_once(uint32_t ep)
        {
            wake_next_park(send_arrives);
            kos_reply_recv_opts opts{};
            ASSERT_EQ(receive(ep, &opts, KOS_RECV_NO_INFO), static_cast<int32_t>(CALL_LEN));
            ASSERT_TRUE(rbuf_holds(0x70u));
        }

        bool rbuf_holds(uint8_t fill)
        {
            for (uint32_t i = 0; i < CALL_LEN; i++)
            {
                if (g_rbuf[i] != static_cast<uint8_t>(fill + i))
                {
                    return false;
                }
            }
            return true;
        }
        }

        // A send that finds its receiver busy stays in its slot until that receiver comes back
        // for it, as a local send waits for a receiver; nothing is answered meanwhile.
        TEST_F(AmpHold, a_far_send_with_no_receiver_parked_waits_in_its_slot_for_one)
        {
            uint32_t ep = KCAP_INVALID;
            Thread* const r = seat_receiver(1, PRIO_RECEIVER, &ep);
            received_once(ep);
            ASSERT_EQ(sched::current(), r);
            uint32_t const answers = answer_ring().head.v[0].load();
            peer_sends(CALL_FILL);
            uint32_t const tail_before = call_ring().tail.v[SERVED.index].load();

            doorbell();

            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), tail_before)
                << "the send was dropped";
            EXPECT_EQ(answer_ring().head.v[0].load(), answers) << "the send was answered unread";

            g_receiver_parked = false;
            wake_next_park(refuse_the_park);
            kos_reply_recv_opts opts{};
            int32_t const rc = receive(ep, &opts, KOS_RECV_NO_INFO);
            EXPECT_FALSE(g_receiver_parked) << "the receiver parked beside a send waiting for it";
            ASSERT_EQ(rc, static_cast<int32_t>(CALL_LEN));
            EXPECT_TRUE(rbuf_holds_the_call(g_rbuf));
            EXPECT_EQ(r->far_hold, 0u);
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), tail_before + 1u)
                << "the landed send kept its slot";
        }

        // Sends waiting for a receiver reach it in the order they were published.
        TEST_F(AmpHold, far_sends_waiting_for_a_receiver_reach_it_in_their_order)
        {
            uint32_t ep = KCAP_INVALID;
            seat_receiver(1, PRIO_RECEIVER, &ep);
            received_once(ep);
            static_assert(SERVED.slots == 2u, "the third send waits for the first's slot");
            peer_sends(0x10u);
            doorbell();
            peer_sends(0x20u);
            doorbell();

            for (uint8_t fill = 0x10u; fill <= 0x30u; fill = static_cast<uint8_t>(fill + 0x10u))
            {
                if (fill == 0x20u)
                {
                    peer_sends(0x30u);
                    doorbell();
                }
                g_receiver_parked = false;
                wake_next_park(refuse_the_park);
                kos_reply_recv_opts opts{};
                ASSERT_EQ(receive(ep, &opts, KOS_RECV_NO_INFO), static_cast<int32_t>(CALL_LEN));
                EXPECT_FALSE(g_receiver_parked);
                EXPECT_TRUE(rbuf_holds(fill)) << "send " << (fill >> 4) << " out of order";
            }
        }

        // The last receiver leaving gives back every send that waited for it, and a send after it
        // is released at once.
        TEST_F(AmpHold, far_sends_waiting_for_a_receiver_go_back_when_the_last_receiver_leaves)
        {
            uint32_t ep = KCAP_INVALID;
            Thread* const r = seat_receiver(1, PRIO_RECEIVER, &ep);
            received_once(ep);
            peer_sends(0x10u);
            peer_sends(0x20u);
            uint32_t const tail_before = call_ring().tail.v[SERVED.index].load();
            doorbell();
            ASSERT_EQ(call_ring().tail.v[SERVED.index].load(), tail_before);

            {
                IrqLock lock;
                ASSERT_EQ(handle_close(r, ep, lock), 0);
            }
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), tail_before + 2u)
                << "a send outlived every receiver of its port, holding its slot";
            peer_sends(CALL_FILL);
            doorbell();
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), tail_before + 3u)
                << "a port with no receiver left kept the send's slot";
        }

        // A send that arrives before its port's server first receives waits for that receive.
        TEST_F(AmpHold, a_far_send_ahead_of_its_servers_first_receive_waits_for_it)
        {
            uint32_t ep = KCAP_INVALID;
            seat_receiver(1, PRIO_RECEIVER, &ep);
            peer_sends(CALL_FILL);
            uint32_t const tail_before = call_ring().tail.v[SERVED.index].load();
            doorbell();
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), tail_before)
                << "the early send was dropped";

            g_receiver_parked = false;
            wake_next_park(refuse_the_park);
            kos_reply_recv_opts opts{};
            ASSERT_EQ(receive(ep, &opts, KOS_RECV_NO_INFO), static_cast<int32_t>(CALL_LEN));
            EXPECT_FALSE(g_receiver_parked);
            EXPECT_TRUE(rbuf_holds_the_call(g_rbuf));
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), tail_before + 1u);
        }

        namespace
        {
            void resync_under_the_hold();

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
        }

        // A port's receivers are each one of its servers: the one that received last leaving
        // gives back nothing held for the port, and the one still receiving takes every send.
        TEST_F(AmpHold, far_sends_held_for_a_port_outlive_one_of_its_two_receivers)
        {
            uint32_t ep = KCAP_INVALID;
            Thread* const first = seat_receiver(1, PRIO_RECEIVER, &ep);
            received_once(ep);
            Thread* const second = seat_pool(2, PRIO_ABOVE);
            attach_caps(second, KICKOS_SEAM_CAP_WIDTH);
            uint32_t const second_ep = give_wait(first, ep, second);
            ASSERT_EQ(sched::current(), first);
            peer_sends(0x10u);
            uint32_t const tail_before = call_ring().tail.v[SERVED.index].load();
            doorbell();

            {
                IrqLock lock;
                ASSERT_EQ(handle_close(first, ep, lock), 0);
            }
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), tail_before)
                << "the first receiver's exit gave back a send the second one serves";
            peer_sends(0x20u);
            doorbell();
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), tail_before)
                << "a send after the first receiver left was refused";

            {
                IrqLock lock;
                sched::reschedule(nullptr, lock);
            }
            ASSERT_EQ(sched::current(), second);
            for (uint8_t fill = 0x10u; fill <= 0x20u; fill = static_cast<uint8_t>(fill + 0x10u))
            {
                g_receiver_parked = false;
                wake_next_park(refuse_the_park);
                kos_reply_recv_opts opts{};
                ASSERT_EQ(receive(second_ep, &opts, KOS_RECV_NO_INFO),
                          static_cast<int32_t>(CALL_LEN));
                EXPECT_FALSE(g_receiver_parked);
                EXPECT_TRUE(rbuf_holds(fill)) << "send " << (fill >> 4) << " out of order";
            }
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), tail_before + 2u);
        }

        // A resynchronisation kills the records of the sends held for a port, and the room they
        // held goes to the sends after it.
        TEST_F(AmpHold, sends_held_after_resynchronisations_killed_every_earlier_one_still_wait)
        {
            uint32_t ep = KCAP_INVALID;
            seat_receiver(1, PRIO_RECEIVER, &ep);
            received_once(ep);
            // Past the held table's size: one held send per node per ring slot.
            for (uint32_t round = 0; round < 2u * amp::NODE_MAX * amp::RING_SLOTS; round++)
            {
                peer_sends(0x10u);
                doorbell();
                resync_under_the_hold();
                // The peer drains the answer the resynchronisation gave the dead send.
                answer_ring().tail.v[0].store(answer_ring().head.v[0].load());
            }
            peer_sends(CALL_FILL);
            uint32_t const tail_before = call_ring().tail.v[SERVED.index].load();
            doorbell();
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), tail_before)
                << "the dead records left no room to hold a later send";

            g_receiver_parked = false;
            wake_next_park(refuse_the_park);
            kos_reply_recv_opts opts{};
            ASSERT_EQ(receive(ep, &opts, KOS_RECV_NO_INFO), static_cast<int32_t>(CALL_LEN));
            EXPECT_FALSE(g_receiver_parked);
            EXPECT_TRUE(rbuf_holds_the_call(g_rbuf));
        }

        // A call that finds its receiver busy waits in its slot for that receiver, as a local
        // call waits for one, and the next receive takes it as a call it answers.
        TEST_F(AmpHold, a_far_call_with_no_receiver_parked_waits_in_its_slot_for_one)
        {
            uint32_t ep = KCAP_INVALID;
            Thread* const r = seat_receiver(1, PRIO_RECEIVER, &ep);
            ASSERT_EQ(sched::current(), r);
            uint32_t const answers = answer_ring().head.v[0].load();
            peer_calls(CALL_FILL);
            uint32_t const tail_before = call_ring().tail.v[SERVED.index].load();

            doorbell();

            EXPECT_EQ(r->far_hold, 0u) << "a running thread was handed the call";
            EXPECT_EQ(answer_ring().head.v[0].load(), answers) << "the call was answered unread";
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), tail_before)
                << "the call gave its slot back unread";

            g_receiver_parked = false;
            wake_next_park(refuse_the_park);
            kos_reply_recv_opts opts{};
            int32_t const rc = receive(ep, &opts, 0);
            EXPECT_FALSE(g_receiver_parked) << "the receiver parked beside a call waiting for it";
            ASSERT_EQ(rc, static_cast<int32_t>(CALL_LEN));
            EXPECT_TRUE(rbuf_holds_the_call(g_rbuf));
            ASSERT_NE(opts.info.reply_cap, KOS_CAP_NONE) << "the call reached its receiver as a send";
            uint8_t const answer[4] = {1u, 2u, 3u, 4u};
            ASSERT_EQ(endpoint_reply(opts.info.reply_cap, reinterpret_cast<uintptr_t>(answer),
                                     sizeof(answer)),
                      0);
            ASSERT_EQ(answer_ring().head.v[0].load(), answers + 1u);
            amp::Slot const& back = answer_ring().slot[answers % amp::RING_SLOTS];
            EXPECT_EQ(back.len.load(), sizeof(answer));
            EXPECT_EQ(back.badge.load(), KOS_BADGE_NONE);
            EXPECT_EQ(back.payload[0], answer[0]);
            EXPECT_EQ(back.tag.seq, PEER_TAG.seq);
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), tail_before + 1u);
        }

        // The port's last receiver leaving refuses the call that waited for it, and a call after
        // it is refused on the spot.
        TEST_F(AmpHold, a_far_call_waiting_for_a_receiver_is_refused_when_the_last_one_leaves)
        {
            uint32_t ep = KCAP_INVALID;
            Thread* const r = seat_receiver(1, PRIO_RECEIVER, &ep);
            uint32_t const answers = answer_ring().head.v[0].load();
            peer_calls(CALL_FILL);
            uint32_t const tail_before = call_ring().tail.v[SERVED.index].load();
            doorbell();
            ASSERT_EQ(answer_ring().head.v[0].load(), answers);

            {
                IrqLock lock;
                ASSERT_EQ(handle_close(r, ep, lock), 0);
            }
            ASSERT_EQ(answer_ring().head.v[0].load(), answers + 1u)
                << "a call outlived every receiver of its port unanswered";
            EXPECT_TRUE(refused_at(answers));
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), tail_before + 1u);

            peer_calls(CALL_FILL);
            doorbell();
            ASSERT_EQ(answer_ring().head.v[0].load(), answers + 2u);
            EXPECT_TRUE(refused_at(answers + 1u));
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), tail_before + 2u);
        }

        namespace
        {
            // The peer withdrawing the call it published last, whose caller has left.
            void peer_withdraws_last()
            {
                amp::Ring& r = call_ring();
                uint32_t const at = r.head.v[SERVED.index].load() - 1u;
                r.slot[SERVED.base + at % SERVED.slots].withdrawn.store(1u);
            }
        }

        // As a local caller that leaves withdraws its request: the call goes back unanswered at
        // the peer's doorbell, and no receiver ever sees it.
        TEST_F(AmpHold, a_held_far_call_its_caller_withdraws_goes_back_unanswered)
        {
            uint32_t ep = KCAP_INVALID;
            seat_receiver(1, PRIO_RECEIVER, &ep);
            uint32_t const answers = answer_ring().head.v[0].load();
            peer_calls(CALL_FILL);
            uint32_t const tail_before = call_ring().tail.v[SERVED.index].load();
            doorbell();
            ASSERT_EQ(call_ring().tail.v[SERVED.index].load(), tail_before);

            peer_withdraws_last();
            doorbell();

            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), tail_before + 1u)
                << "the withdrawn call kept its slot";
            EXPECT_EQ(answer_ring().head.v[0].load(), answers) << "a withdrawn call was answered";
            g_receiver_parked = false;
            wake_next_park(refuse_the_park);
            kos_reply_recv_opts opts{};
            EXPECT_EQ(receive(ep, &opts, 0), -KOS_ETIMEDOUT);
            EXPECT_TRUE(g_receiver_parked) << "a receiver was handed a withdrawn call";
        }

        // A receive meeting a withdrawn call before the doorbell's sweep passes over it too.
        TEST_F(AmpHold, a_receive_passes_over_a_held_call_its_caller_withdrew)
        {
            uint32_t ep = KCAP_INVALID;
            seat_receiver(1, PRIO_RECEIVER, &ep);
            uint32_t const answers = answer_ring().head.v[0].load();
            peer_calls(CALL_FILL);
            uint32_t const tail_before = call_ring().tail.v[SERVED.index].load();
            doorbell();
            peer_withdraws_last();

            g_receiver_parked = false;
            wake_next_park(refuse_the_park);
            kos_reply_recv_opts opts{};
            EXPECT_EQ(receive(ep, &opts, 0), -KOS_ETIMEDOUT);
            EXPECT_TRUE(g_receiver_parked) << "a receiver was handed a withdrawn call";
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), tail_before + 1u);
            EXPECT_EQ(answer_ring().head.v[0].load(), answers);
        }

        TEST_F(AmpHold, a_far_call_withdrawn_before_its_take_goes_back_unanswered)
        {
            uint32_t ep = KCAP_INVALID;
            seat_receiver(1, PRIO_RECEIVER, &ep);
            uint32_t const answers = answer_ring().head.v[0].load();
            peer_calls(CALL_FILL);
            uint32_t const tail_before = call_ring().tail.v[SERVED.index].load();
            peer_withdraws_last();

            doorbell();

            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), tail_before + 1u);
            EXPECT_EQ(answer_ring().head.v[0].load(), answers);
            g_receiver_parked = false;
            wake_next_park(refuse_the_park);
            kos_reply_recv_opts opts{};
            EXPECT_EQ(receive(ep, &opts, 0), -KOS_ETIMEDOUT);
            EXPECT_TRUE(g_receiver_parked);
        }

        namespace
        {
            void withdrawn_after_the_delivery()
            {
                peer_withdraws_last();
                doorbell();
            }
        }

        // A call already handed to its receiver stays taken, as a local one does: the
        // withdrawal releases nothing, and the reply is published once.
        TEST_F(AmpHold, a_far_call_withdrawn_after_its_receiver_took_it_stays_taken)
        {
            uint32_t ep = KCAP_INVALID;
            seat_receiver(1, PRIO_RECEIVER, &ep);
            uint32_t const answers = answer_ring().head.v[0].load();
            g_after_service = withdrawn_after_the_delivery;
            wake_next_park(call_arrives);

            kos_reply_recv_opts opts{};
            ASSERT_EQ(receive(ep, &opts, 0), static_cast<int32_t>(CALL_LEN));
            EXPECT_TRUE(rbuf_holds_the_call(g_rbuf));
            ASSERT_NE(opts.info.reply_cap, KOS_CAP_NONE);
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), g_tail_before)
                << "the withdrawal released a slot its receiver holds";
            uint8_t const answer[1] = {0x33u};
            ASSERT_EQ(endpoint_reply(opts.info.reply_cap, reinterpret_cast<uintptr_t>(answer),
                                     sizeof(answer)),
                      0);
            EXPECT_EQ(answer_ring().head.v[0].load(), answers + 1u);
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), g_tail_before + 1u);
        }

        TEST_F(AmpHold, a_far_call_to_a_port_bound_to_nothing_is_refused_and_released)
        {
            uint32_t const answers = answer_ring().head.v[0].load();
            peer_calls(CALL_FILL);
            uint32_t const tail_before = call_ring().tail.v[SERVED.index].load();

            doorbell();

            ASSERT_EQ(answer_ring().head.v[0].load(), answers + 1u)
                << "the far caller of a port nobody serves is never answered";
            EXPECT_TRUE(refused_at(answers));
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), tail_before + 1u)
                << "the refused call kept its slot";
        }

        TEST_F(AmpHold, a_receive_whose_info_write_is_refused_takes_back_the_far_reply_capability)
        {
            uint32_t ep = KCAP_INVALID;
            Thread* const r = seat_receiver(1, PRIO_RECEIVER, &ep);
            wake_next_park(call_arrives);
            uint32_t const answers = answer_ring().head.v[0].load();
            uint32_t const faults = amp::counts(ME).deliver_fault.load();
            kos_reply_recv_opts refused{};
            g_ipc_seam_refuse_write = reinterpret_cast<uintptr_t>(&refused.info);

            int32_t const rc = receive(ep, &refused, 0);
            g_ipc_seam_refuse_write = 0;

            ASSERT_EQ(rc, -KOS_EFAULT);
            EXPECT_EQ(refused.info.reply_cap, KOS_CAP_NONE);
            EXPECT_EQ(cap_reply_live(r), 0u)
                << "the capability the receiver was never told of stayed installed";
            EXPECT_EQ(amp::counts(ME).deliver_fault.load(), faults + 1u);
            ASSERT_EQ(answer_ring().head.v[0].load(), answers + 1u)
                << "the far caller is unanswered";
            EXPECT_TRUE(answered_empty_at(answers));
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), g_tail_before + 1u)
                << "the slot was never released";

            wake_next_park(call_arrives);
            kos_reply_recv_opts opts{};
            ASSERT_EQ(receive(ep, &opts, 0), static_cast<int32_t>(CALL_LEN));
            EXPECT_NE(opts.info.reply_cap, KOS_CAP_NONE) << "the control is handed no capability";
            EXPECT_EQ(cap_reply_live(r), 1u);
            // A far record outlives amp_reset; left live, a later case's resynchronisation
            // answers it.
            EXPECT_EQ(endpoint_reply(opts.info.reply_cap, reinterpret_cast<uintptr_t>(g_rbuf), 0u),
                      0);
        }

        TEST_F(AmpHold, no_far_reply_band_handle_resolves_to_a_local_thread)
        {
            for (int i = 0; i < KICKOS_SEAM_THREAD_SLOTS; i++)
            {
                Thread* const t = seat_pool(i, PRIO_RECEIVER);
                IrqLock lock;
                testfix::seat_blocked(t);
                policy_on_remove(t);
                t->call_state = CALL_REPLY_WAIT;
            }
            ASSERT_EQ(kernel().threads.next, KICKOS_SEAM_THREAD_SLOTS);

            IrqLock lock;
            for (uint32_t rec = 0; rec < ThreadPool::FAR_REPLY_RECORDS; rec++)
            {
                uint32_t const handle = ThreadPool::far_reply_handle(rec);
                EXPECT_GE(handle_index(handle), static_cast<uint32_t>(kernel().threads.next))
                    << "record " << rec << " lies inside the pool's high-water mark";
                EXPECT_EQ(cap_reply_thread(handle, 0u, 0u), nullptr)
                    << "record " << rec << " resolved to a local thread";
            }
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
            kos_reply_recv_opts_init(&opts, ep, 0, KOS_NEVER);
            ASSERT_NO_FATAL_FAILURE(park_receiver(r, ep, &opts));

            peer_calls(CALL_FILL);
            uint32_t const tail_before = call_ring().tail.v[SERVED.index].load();
            uint32_t const answers = answer_ring().head.v[0].load();
            doorbell();
            ASSERT_EQ(r->state, ThreadState::READY);
            ASSERT_NE(r->far_hold, 0u);
            ASSERT_EQ(call_ring().tail.v[SERVED.index].load(), tail_before);

            {
                IrqLock lock;
                thread_cancel_kind(r, CANCEL_SLAY, lock);
                sched::set_prio(above, PRIO_BELOW, lock);
                sched::reschedule(nullptr, lock);
            }
            ASSERT_EQ(sched::current(), r);
            ASSERT_EQ(g_redirect_target, r) << "the slay claim did not take the resume";
            run_noreturn(slay_exit);

            EXPECT_TRUE(rbuf_untouched(g_rbuf)) << "a landing ran for a thread that was slain";
            EXPECT_EQ(r->far_hold, 0u);
            ASSERT_EQ(answer_ring().head.v[0].load(), answers + 1u)
                << "the far caller of a slain receiver is never answered";
            EXPECT_TRUE(answered_empty_at(answers));
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load(), tail_before + 1u);
        }

        namespace
        {
            // A depth no lane can hold, standing for the whole strike bound, which resynchronises
            // the lane under the record the receiver has not landed yet. A whole multiple of the
            // lane's width, so the adopted tail masks back onto the held slot.
            void resync_under_the_hold()
            {
                amp::Ring& r = call_ring();
                uint32_t const tail = r.tail.v[SERVED.index].load();
                r.head.v[SERVED.index].store(tail + 2u * amp::RING_SLOTS);
                for (uint32_t i = 0; i < amp::DEPTH_STRIKES; i++)
                {
                    amp::node_service(kickos::IrqLock());
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
            uint32_t const answers = answer_ring().head.v[0].load();
            g_after_service = resync_under_the_hold;
            wake_next_park(call_arrives);

            kos_reply_recv_opts opts{};
            int32_t const rc = receive(ep, &opts, 0);

            uint32_t const resynced = call_ring().head.v[SERVED.index].load();
            ASSERT_EQ(call_ring().tail.v[SERVED.index].load(), resynced)
                << "the resynchronisation never ran";
            EXPECT_EQ(rc, -KOS_EPIPE);
            EXPECT_TRUE(rbuf_untouched(g_rbuf));
            EXPECT_EQ(opts.info.reply_cap, KOS_CAP_NONE);
            EXPECT_EQ(answer_ring().head.v[0].load(), answers + 1u)
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
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load() + 1u,
                      call_ring().head.v[SERVED.index].load())
                << "the old receiver released the later call's slot";
        }

        namespace
        {
            bool g_old_hold_live = true;
            Thread* g_second = nullptr;
            uint32_t g_second_ep = KCAP_INVALID;
            kos_reply_recv_opts g_second_opts{};

            void reset_reuse_and_probe_old_hold()
            {
                resync_under_the_hold();
                ASSERT_NO_FATAL_FAILURE(park_receiver(g_second, g_second_ep, &g_second_opts));
                g_second->far_hold = 0u;
                peer_calls(0x90u);
                amp::node_service(kickos::IrqLock());
                ASSERT_NE(g_second->far_hold, 0u) << "the reused slot was never seated";
                g_old_hold_live = amp::hold_live(g_hold_at_service - 1u);
            }
        }

        TEST_F(AmpHold, a_hold_a_reset_freed_is_not_live_once_its_slot_is_seated_again)
        {
            uint32_t ep = KCAP_INVALID;
            g_second = seat_receiver(1, PRIO_RECEIVER, &ep);
            g_second_ep = ep;
            kos_reply_recv_opts_init(&g_second_opts, ep, 0, KOS_NEVER);
            g_old_hold_live = true;
            g_after_service = reset_reuse_and_probe_old_hold;
            wake_next_park(call_arrives);

            kos_reply_recv_opts opts{};
            receive(ep, &opts, 0);

            ASSERT_NE(g_hold_at_service, 0u);
            EXPECT_FALSE(g_old_hold_live);
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
            EXPECT_EQ(call_ring().tail.v[SERVED.index].load() + 1u,
                      call_ring().head.v[SERVED.index].load())
                << "the old receiver released the later call's slot";
        }

        // --- the reply side ------------------------------------------------------------
        namespace
        {
            constexpr uint32_t PORT_FAR = 3; // the partition's entry on the peer
            constexpr amp::Lane OUTBOUND = amp::lane_of(amp::Class::CALL, PEER, PORT_FAR);
            constexpr amp::Lane OUTBOUND_ECHO =
                amp::lane_of(amp::Class::CALL, PEER, amp::PORT_ECHO);
            // The lane of the outbound ring the call a reply answers went out on.
            amp::Lane g_call_lane = OUTBOUND;
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

            void peer_answers(amp::ReplyTag const& tag, uint32_t len, uint32_t word)
            {
                amp::Ring& r = reply_ring();
                uint32_t const head = r.head.v[0].load();
                amp::Slot& s = r.slot[head % amp::RING_SLOTS];
                s.port.store(amp::PORT_REPLY);
                s.tag = tag;
                s.badge.store(word);
                s.len.store(len);
                for (uint32_t i = 0; i < len; i++)
                {
                    s.payload[i] = static_cast<uint8_t>(REPLY_FILL + i);
                }
                r.head.v[0].store(head + 1u);
            }

            void peer_replies_len(amp::ReplyTag const& tag, uint32_t len)
            {
                peer_answers(tag, len, KOS_BADGE_NONE);
            }

            void peer_replies(amp::ReplyTag const& tag)
            {
                peer_replies_len(tag, REPLY_LEN);
            }

            // The route the caller's last publication carried, as the peer would hand it back.
            amp::ReplyTag published_tag()
            {
                amp::Ring const& r = outbound_ring();
                uint32_t const at = r.head.v[g_call_lane.index].load() - 1u;
                return r.slot[g_call_lane.base + at % g_call_lane.slots].tag;
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
                g_reply_tail_before = reply_ring().tail.v[0].load();
                amphold::g_raise_mask = 0;
                amp::node_service(kickos::IrqLock());
                memcpy(g_cbuf_at_service, g_cbuf, sizeof(g_cbuf));
                g_hold_at_service = parked->far_hold;
                g_reply_tail_at_service = reply_ring().tail.v[0].load();
                g_credit_at_service = amphold::g_raise_mask & PEER_CORE_BIT;
                if (g_after_service != nullptr)
                {
                    g_after_service();
                }
            }

            Thread* g_caller = nullptr;

            void kill_after_the_hold()
            {
                thread_cancel_kind(g_caller, CANCEL_KILL, kickos::IrqLock());
            }

            // The deadline fires while the caller waits, and the reply lands after it.
            void timed_out_then_reply_arrives(Thread* parked)
            {
                amp::ReplyTag const tag = published_tag();
                thread_abort_park(parked, -KOS_ETIMEDOUT, kickos::IrqLock());
                peer_replies(tag);
                g_reply_tail_before = reply_ring().tail.v[0].load();
                amphold::g_raise_mask = 0;
                amp::node_service(kickos::IrqLock());
                g_hold_at_service = parked->far_hold;
                g_reply_tail_at_service = reply_ring().tail.v[0].load();
                g_credit_at_service = amphold::g_raise_mask & PEER_CORE_BIT;
            }

            // A pool thread running, holding a far endpoint on the peer's port.
            Thread* seat_caller(int slot, uint8_t prio, uint32_t* out_far)
            {
                Thread* const c = seat_pool(slot, prio);
                attach_caps(c, KICKOS_SEAM_CAP_WIDTH);
                IrqLock lock;
                sched::reschedule(nullptr, lock);
                EXPECT_EQ(amp_endpoint_mint(c, PEER, PORT_FAR, CAP_SIGNAL, out_far), 0);
                return c;
            }

            int32_t call(uint32_t far)
            {
                return endpoint_call(far, reinterpret_cast<uintptr_t>(g_cbuf), CALL_LEN, RECV_CAP,
                                     KOS_LIMIT_NONE);
            }

            class AmpReplyHold : public AmpHold
            {
              protected:
                void SetUp() override
                {
                    AmpHold::SetUp();
                    poison_cbuf();
                    g_caller = nullptr;
                    g_call_lane = OUTBOUND;
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
            EXPECT_EQ(reply_ring().tail.v[0].load(), g_reply_tail_before + 1u)
                << "the landing never released the slot";
            EXPECT_EQ(amphold::g_raise_mask & PEER_CORE_BIT, PEER_CORE_BIT)
                << "the release that freed the slot returned no credit";
        }

        namespace
        {
            void empty_reply_arrives(Thread*)
            {
                peer_replies_len(published_tag(), 0u);
                g_reply_tail_before = reply_ring().tail.v[0].load();
                amphold::g_raise_mask = 0;
                amp::node_service(kickos::IrqLock());
            }
        }

        namespace
        {
            void refusal_arrives(Thread*)
            {
                peer_answers(published_tag(), 0u, amp::REPLY_REFUSED);
                g_reply_tail_before = reply_ring().tail.v[0].load();
                amphold::g_raise_mask = 0;
                amp::node_service(kickos::IrqLock());
            }
        }

        namespace
        {
            amp::Slot const& published_slot()
            {
                amp::Ring const& r = outbound_ring();
                uint32_t const at = r.head.v[g_call_lane.index].load() - 1u;
                return r.slot[g_call_lane.base + at % g_call_lane.slots];
            }

            void caller_leaves(Thread* parked)
            {
                amphold::g_raise_mask = 0;
                thread_abort_park(parked, -KOS_ETIMEDOUT, kickos::IrqLock());
            }

            void slot_back_then_caller_leaves(Thread* parked)
            {
                amp::Ring& r = outbound_ring();
                r.tail.v[g_call_lane.index].store(r.head.v[g_call_lane.index].load());
                caller_leaves(parked);
            }
        }

        // A caller that leaves its far call withdraws the request and rings the serving node.
        TEST_F(AmpReplyHold, a_far_caller_that_leaves_withdraws_its_call)
        {
            uint32_t far = KCAP_INVALID;
            seat_caller(1, PRIO_RECEIVER, &far);
            wake_next_park(caller_leaves);

            EXPECT_EQ(call(far), -KOS_ETIMEDOUT);
            EXPECT_EQ(published_slot().withdrawn.load(), 1u) << "the call was left to be served";
            EXPECT_EQ(amphold::g_raise_mask & PEER_CORE_BIT, PEER_CORE_BIT);
        }

        // The control: a slot the serving node already gave back is not touched.
        TEST_F(AmpReplyHold, a_far_caller_leaving_after_its_slot_came_back_withdraws_nothing)
        {
            uint32_t far = KCAP_INVALID;
            seat_caller(1, PRIO_RECEIVER, &far);
            wake_next_park(slot_back_then_caller_leaves);

            EXPECT_EQ(call(far), -KOS_ETIMEDOUT);
            EXPECT_EQ(published_slot().withdrawn.load(), 0u);
        }

        // The serving node's refusal reaches the caller as a local call's does.
        TEST_F(AmpReplyHold, a_refused_far_call_answers_econnrefused_and_releases_the_slot)
        {
            uint32_t far = KCAP_INVALID;
            Thread* const c = seat_caller(1, PRIO_RECEIVER, &far);
            wake_next_park(refusal_arrives);
            uint32_t const drops = amp::counts(ME).reply_drop.load();

            int32_t const rc = call(far);

            EXPECT_EQ(rc, -KOS_ECONNREFUSED);
            EXPECT_TRUE(cbuf_holds(g_cbuf, REQUEST_FILL, CALL_LEN));
            EXPECT_EQ(c->far_hold, 0u);
            EXPECT_EQ(amp::counts(ME).reply_drop.load(), drops);
            EXPECT_EQ(reply_ring().tail.v[0].load(), g_reply_tail_before + 1u)
                << "the refusal's slot was never released";
            EXPECT_EQ(amphold::g_raise_mask & PEER_CORE_BIT, PEER_CORE_BIT);
        }

        TEST_F(AmpReplyHold, a_zero_length_far_reply_completes_the_call_with_zero)
        {
            uint32_t far = KCAP_INVALID;
            Thread* const c = seat_caller(1, PRIO_RECEIVER, &far);
            wake_next_park(empty_reply_arrives);
            uint32_t const drops = amp::counts(ME).reply_drop.load();

            int32_t const rc = call(far);

            EXPECT_EQ(rc, 0);
            EXPECT_TRUE(cbuf_holds(g_cbuf, REQUEST_FILL, CALL_LEN));
            EXPECT_EQ(c->far_hold, 0u);
            EXPECT_EQ(amp::counts(ME).reply_drop.load(), drops);
            EXPECT_EQ(reply_ring().tail.v[0].load(), g_reply_tail_before + 1u)
                << "the empty reply's slot was never released";
            EXPECT_EQ(amphold::g_raise_mask & PEER_CORE_BIT, PEER_CORE_BIT);
        }

        namespace
        {
            void resync_and_reuse_reply_slot()
            {
                amp::Ring& r = reply_ring();
                r.head.v[0].store(r.tail.v[0].load() + 2u * amp::RING_SLOTS);
                for (uint32_t i = 0; i < amp::DEPTH_STRIKES; i++)
                {
                    amp::node_service(kickos::IrqLock());
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
            EXPECT_EQ(reply_ring().tail.v[0].load() + 1u, reply_ring().head.v[0].load())
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
            EXPECT_EQ(reply_ring().tail.v[0].load() + 1u, reply_ring().head.v[0].load())
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
            EXPECT_EQ(reply_ring().tail.v[0].load(), g_reply_tail_before + 1u);
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
            EXPECT_EQ(reply_ring().tail.v[0].load(), g_reply_tail_before + 1u);
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
                testfix::seat_blocked(c);
                policy_on_remove(c);
                c->wait_queue = nullptr;
                c->wait_kind = WAIT_EP_FAR_REPLY;
                c->wait_obj = e;
                c->wait_result = WAIT_RESULT_POISON;
                return tag;
            }

            // A far caller parked on its call, behind a running thread parked on nothing.
            Thread* parked_behind(Thread** out_above, amp::ReplyTag* out_tag)
            {
                *out_above = seat_pool(0, PRIO_ABOVE);
                uint32_t far = KCAP_INVALID;
                Thread* const c = seat_caller(1, PRIO_RECEIVER, &far);
                EXPECT_EQ(sched::current(), *out_above);
                *out_tag = park_far_caller(c, far);
                return c;
            }

            // A caller holding its reply, READY behind a thread that outranks it.
            Thread* held_behind(Thread** out_above)
            {
                amp::ReplyTag tag = {};
                Thread* const c = parked_behind(out_above, &tag);
                peer_replies(tag);
                g_reply_tail_before = reply_ring().tail.v[0].load();
                doorbell();
                EXPECT_EQ(c->state, ThreadState::READY);
                EXPECT_NE(c->far_hold, 0u);
                EXPECT_EQ(reply_ring().tail.v[0].load(), g_reply_tail_before);
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
                thread_cancel_kind(c, CANCEL_SLAY, lock);
                sched::set_prio(above, PRIO_BELOW, lock);
                sched::reschedule(nullptr, lock);
            }
            ASSERT_EQ(sched::current(), c);
            ASSERT_EQ(g_redirect_target, c) << "the slay claim did not take the resume";
            run_noreturn(slay_exit);

            EXPECT_TRUE(cbuf_holds(g_cbuf, REQUEST_FILL, CALL_LEN))
                << "a landing ran for a thread that was slain";
            EXPECT_EQ(c->far_hold, 0u);
            EXPECT_EQ(reply_ring().tail.v[0].load(), g_reply_tail_before + 1u)
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
                sched::set_prio(above, PRIO_BELOW, lock);
                sched::reschedule(nullptr, lock);
            }
            ASSERT_EQ(sched::current(), c);
            run_noreturn(exit_now);

            EXPECT_EQ(c->far_hold, 0u);
            EXPECT_EQ(reply_ring().tail.v[0].load(), g_reply_tail_before + 1u)
                << "an exiting caller's reply slot is never given back";
            EXPECT_EQ(amphold::g_raise_mask & PEER_CORE_BIT, PEER_CORE_BIT);
        }

        // --- a full call ring toward the peer ----------------------------------------------
        // A blocking far send or call that meets every slot of the ring outstanding waits for
        // one, as a blocking local send waits for a receiver; KOS_POLL is refused.
        namespace
        {
            constexpr uint64_t DEADLINE_POLL = 0u;
            constexpr uint64_t NOW = 1000u;
            // A third node, and the port the partition names on it.
            constexpr uint32_t OTHER = 2;
            constexpr uint32_t PORT_OTHER = 4;
            constexpr amp::Lane OTHER_LANE = amp::lane_of(amp::Class::CALL, OTHER, PORT_OTHER);

            int32_t send_far(uint32_t far, uint64_t deadline)
            {
                return endpoint_send(far, reinterpret_cast<uintptr_t>(g_cbuf), CALL_LEN,
                                     deadline);
            }

            int32_t call_far(uint32_t far, uint64_t deadline)
            {
                return endpoint_call(far, reinterpret_cast<uintptr_t>(g_cbuf), CALL_LEN, RECV_CAP,
                                     deadline);
            }

            uint32_t outbound_used()
            {
                amp::Ring const& r = outbound_ring();
                return r.head.v[OUTBOUND.index].load() - r.tail.v[OUTBOUND.index].load();
            }

            amp::Ring& other_ring()
            {
                return amp::ring_for(amp::Class::CALL, OTHER, ME);
            }

            // The peer takes nothing, so every slot of the port's lane toward it stays outstanding.
            void fill_outbound(uint32_t far)
            {
                for (uint32_t i = 0; i < OUTBOUND.slots; i++)
                {
                    ASSERT_EQ(send_far(far, DEADLINE_POLL), static_cast<int32_t>(CALL_LEN))
                        << "publication " << i;
                }
                ASSERT_EQ(outbound_used(), OUTBOUND.slots);
            }

            void fill_other()
            {
                amp::Ring& r = other_ring();
                r.head.v[OTHER_LANE.index].store(r.tail.v[OTHER_LANE.index].load()
                                                 + OTHER_LANE.slots);
            }

            // The peer gives the oldest slot back and its doorbell lands here.
            void peer_frees_a_slot(Thread*)
            {
                amp::Ring& r = outbound_ring();
                r.tail.v[OUTBOUND.index].store(r.tail.v[OUTBOUND.index].load() + 1u);
                kickos_amp_node_service();
            }

            void other_frees_a_slot()
            {
                amp::Ring& r = other_ring();
                r.tail.v[OTHER_LANE.index].store(r.tail.v[OTHER_LANE.index].load() + 1u);
                kickos_amp_node_service();
            }

            void peer_frees_a_slot_then_replies(Thread* parked)
            {
                peer_frees_a_slot(parked);
                wake_next_park(reply_arrives);
            }

            void unrelated_doorbell_then_peer_frees_a_slot(Thread* parked)
            {
                kickos_amp_node_service();
                EXPECT_EQ(parked->state, ThreadState::BLOCKED)
                    << "a doorbell that freed no slot woke the sender";
                peer_frees_a_slot(parked);
            }

            void deadline_passes(Thread* parked)
            {
                thread_abort_park(parked, -KOS_ETIMEDOUT, kickos::IrqLock());
            }

            void killed_while_waiting(Thread* parked)
            {
                thread_cancel_kind(parked, CANCEL_KILL, kickos::IrqLock());
            }

            bool published_at(uint32_t index, amp::ReplyTag const& tag)
            {
                amp::Slot const& s = outbound_ring().slot[OUTBOUND.base + index % OUTBOUND.slots];
                return s.port.load() == PORT_FAR and s.len.load() == CALL_LEN
                       and s.tag.thread == tag.thread and s.tag.seq == tag.seq
                       and cbuf_holds(s.payload, REQUEST_FILL, CALL_LEN);
            }
        }

        TEST_F(AmpReplyHold, a_far_send_under_poll_to_a_full_ring_is_refused_with_eagain)
        {
            uint32_t far = KCAP_INVALID;
            g_caller = seat_caller(1, PRIO_RECEIVER, &far);
            fill_outbound(far);
            ASSERT_FALSE(HasFailure());
            uint32_t const head = outbound_ring().head.v[OUTBOUND.index].load();
            uint32_t const refused = amp::counts(ME).send_refused.load();

            EXPECT_EQ(send_far(far, DEADLINE_POLL), -KOS_EAGAIN);
            EXPECT_EQ(outbound_ring().head.v[OUTBOUND.index].load(), head);
            EXPECT_EQ(amp::counts(ME).send_refused.load(), refused + 1u);
            EXPECT_EQ(g_caller->wait_kind, WAIT_NONE);
        }

        // The request's form decides the answer: a deadline that has passed is not KOS_POLL.
        TEST_F(AmpReplyHold, a_far_send_whose_deadline_has_passed_meets_a_full_ring_with_etimedout)
        {
            uint32_t far = KCAP_INVALID;
            g_caller = seat_caller(1, PRIO_RECEIVER, &far);
            fill_outbound(far);
            ASSERT_FALSE(HasFailure());
            g_now_ns = NOW;
            uint32_t const head = outbound_ring().head.v[OUTBOUND.index].load();
            uint32_t const refused = amp::counts(ME).send_refused.load();

            EXPECT_EQ(send_far(far, NOW), -KOS_ETIMEDOUT);
            EXPECT_EQ(outbound_ring().head.v[OUTBOUND.index].load(), head);
            EXPECT_EQ(amp::counts(ME).send_refused.load(), refused);
            EXPECT_EQ(g_caller->wait_kind, WAIT_NONE);
        }

        // A call always waits for its reply, so a passed deadline answers as a local call does:
        // KOS_POLL is refused, any other times out.
        TEST_F(AmpReplyHold, a_far_call_whose_deadline_has_passed_publishes_nothing)
        {
            uint32_t far = KCAP_INVALID;
            g_caller = seat_caller(1, PRIO_RECEIVER, &far);
            fill_outbound(far);
            ASSERT_FALSE(HasFailure());
            g_now_ns = NOW;
            poison_cbuf();
            uint32_t const head = outbound_ring().head.v[OUTBOUND.index].load();
            uint16_t const seq = g_caller->call_seq;
            uint32_t const refused = amp::counts(ME).send_refused.load();

            EXPECT_EQ(call_far(far, NOW), -KOS_ETIMEDOUT);
            EXPECT_EQ(call_far(far, DEADLINE_POLL), -KOS_EAGAIN);
            EXPECT_EQ(outbound_ring().head.v[OUTBOUND.index].load(), head);
            EXPECT_EQ(g_caller->call_seq, seq);
            EXPECT_EQ(amp::counts(ME).send_refused.load(), refused);
            EXPECT_EQ(g_caller->wait_kind, WAIT_NONE);
            EXPECT_TRUE(cbuf_holds(g_cbuf, REQUEST_FILL, CALL_LEN));
        }

        // The local rule is the far one: KOS_POLL with no receiver parked is refused, any other
        // passed deadline times out.
        TEST_F(AmpReplyHold, a_polled_local_send_or_call_with_no_receiver_parked_answers_eagain)
        {
            uint32_t far = KCAP_INVALID;
            g_caller = seat_caller(1, PRIO_RECEIVER, &far);
            uint32_t local = KCAP_INVALID;
            ASSERT_EQ(endpoint_create(&local), 0);
            g_now_ns = NOW;

            EXPECT_EQ(send_far(local, DEADLINE_POLL), -KOS_EAGAIN);
            EXPECT_EQ(send_far(local, NOW), -KOS_ETIMEDOUT);
            EXPECT_EQ(call_far(local, DEADLINE_POLL), -KOS_EAGAIN);
            EXPECT_EQ(call_far(local, NOW), -KOS_ETIMEDOUT);
            EXPECT_EQ(g_caller->wait_kind, WAIT_NONE);
        }

        // A receive finding no sender answers as a send finding no receiver.
        TEST_F(AmpReplyHold, a_receive_under_poll_with_no_sender_is_refused_with_eagain)
        {
            uint32_t far = KCAP_INVALID;
            g_caller = seat_caller(1, PRIO_RECEIVER, &far);
            uint32_t local = KCAP_INVALID;
            ASSERT_EQ(endpoint_create(&local), 0);
            g_now_ns = NOW;

            for (uint64_t const until : {DEADLINE_POLL, NOW})
            {
                kos_reply_recv_opts opts{};
                kos_reply_recv_opts_init(&opts, local, KOS_RECV_NO_INFO,
                                         kos_monotonic_t{static_cast<int64_t>(until)});
                int32_t const rc = endpoint_reply_recv(
                    KOS_CAP_NONE, reinterpret_cast<uintptr_t>(g_rbuf),
                    kos_call_lens_pack(0, RECV_CAP), reinterpret_cast<uintptr_t>(&opts));
                if (until == DEADLINE_POLL)
                {
                    EXPECT_EQ(rc, -KOS_EAGAIN);
                }
                else
                {
                    EXPECT_EQ(rc, -KOS_ETIMEDOUT);
                }
            }
            EXPECT_EQ(g_caller->wait_kind, WAIT_NONE);
        }

        // Nobody serves an endpoint whose last receiver left while a HANDOUT holder remains: a
        // send or a call is refused at once, whatever its deadline.
        TEST_F(AmpReplyHold, a_send_or_call_nobody_serves_is_refused_with_econnrefused)
        {
            uint32_t far = KCAP_INVALID;
            g_caller = seat_caller(1, PRIO_RECEIVER, &far);
            uint32_t local = KCAP_INVALID;
            ASSERT_EQ(endpoint_create(&local), 0);
            {
                IrqLock lock;
                ASSERT_EQ(cap_narrow(g_caller, local, CAP_SIGNAL | CAP_HANDOUT, lock), 0);
            }
            g_now_ns = NOW;

            EXPECT_EQ(send_far(local, KOS_LIMIT_NONE), -KOS_ECONNREFUSED);
            EXPECT_EQ(send_far(local, DEADLINE_POLL), -KOS_ECONNREFUSED);
            EXPECT_EQ(send_far(local, NOW), -KOS_ECONNREFUSED);
            EXPECT_EQ(call_far(local, KOS_LIMIT_NONE), -KOS_ECONNREFUSED);
            EXPECT_EQ(call_far(local, DEADLINE_POLL), -KOS_ECONNREFUSED);
            EXPECT_EQ(g_caller->wait_kind, WAIT_NONE);
        }

        namespace
        {
            uint32_t g_live_ep = KCAP_INVALID;
            int32_t g_live_rc = 0;

            void live_receiver_takes_and_answers(Thread*)
            {
                kos_reply_recv_opts opts{};
                g_live_rc = receive(g_live_ep, &opts, 0u);
                if (opts.info.reply_cap != KOS_CAP_NONE)
                {
                    EXPECT_EQ(endpoint_reply(opts.info.reply_cap,
                                             reinterpret_cast<uintptr_t>(g_rbuf), 1u),
                              0);
                }
            }

            // A receiver parked on the endpoint `local` names in g_caller's table, and dying.
            Thread* park_dying_receiver(uint32_t local)
            {
                Thread* const dying = seat_pool(2, PRIO_BELOW);
                attach_caps(dying, KICKOS_SEAM_CAP_WIDTH);
                uint32_t const ep = give_wait(g_caller, local, dying);
                static kos_reply_recv_opts opts;
                opts = {};
                park_receiver(dying, ep, &opts);
                dying->dying = true;
                return dying;
            }
        }

        // A dying receiver at the head of the queue refuses no call another receiver of the
        // endpoint can take: the call waits, and the live one serves it.
        TEST_F(AmpReplyHold, a_call_meeting_a_dying_receiver_is_served_by_a_live_one)
        {
            uint32_t far = KCAP_INVALID;
            g_caller = seat_caller(1, PRIO_RECEIVER, &far);
            uint32_t local = KCAP_INVALID;
            ASSERT_EQ(endpoint_create(&local), 0);
            ASSERT_NO_FATAL_FAILURE(park_dying_receiver(local));
            Thread* const live = seat_pool(3, PRIO_BELOW);
            attach_caps(live, KICKOS_SEAM_CAP_WIDTH);
            g_live_ep = give_wait(g_caller, local, live);
            g_live_rc = 0;
            wake_next_park(live_receiver_takes_and_answers);

            EXPECT_EQ(call_far(local, KOS_LIMIT_NONE), 1);
            EXPECT_EQ(g_live_rc, static_cast<int32_t>(CALL_LEN)) << "the live one took nothing";
        }

        // The control: with the dying receiver the endpoint's last, nobody can serve the call.
        TEST_F(AmpReplyHold, a_call_meeting_its_endpoints_only_receiver_dying_is_refused)
        {
            uint32_t far = KCAP_INVALID;
            g_caller = seat_caller(1, PRIO_RECEIVER, &far);
            uint32_t local = KCAP_INVALID;
            ASSERT_EQ(endpoint_create(&local), 0);
            ASSERT_NO_FATAL_FAILURE(park_dying_receiver(local));
            {
                IrqLock lock;
                ASSERT_EQ(cap_narrow(g_caller, local, CAP_SIGNAL | CAP_HANDOUT, lock), 0);
            }

            EXPECT_EQ(call_far(local, KOS_LIMIT_NONE), -KOS_ECONNREFUSED);
            EXPECT_EQ(g_caller->wait_kind, WAIT_NONE);
        }

        TEST_F(AmpReplyHold, a_blocking_far_send_to_a_full_ring_waits_for_a_slot_and_publishes)
        {
            uint32_t far = KCAP_INVALID;
            g_caller = seat_caller(1, PRIO_RECEIVER, &far);
            fill_outbound(far);
            ASSERT_FALSE(HasFailure());
            uint32_t const head = outbound_ring().head.v[OUTBOUND.index].load();
            uint32_t const refused = amp::counts(ME).send_refused.load();
            wake_next_park(unrelated_doorbell_then_peer_frees_a_slot);

            int32_t const rc = send_far(far, KOS_LIMIT_NONE);

            EXPECT_EQ(rc, static_cast<int32_t>(CALL_LEN));
            EXPECT_EQ(outbound_ring().head.v[OUTBOUND.index].load(), head + 1u);
            EXPECT_TRUE(published_at(head, amp::REPLY_TAG_NONE));
            EXPECT_EQ(g_caller->wait_kind, WAIT_NONE);
            EXPECT_EQ(amp::counts(ME).send_refused.load(), refused)
                << "a wait for a slot was counted as a refusal";
        }

        TEST_F(AmpReplyHold, a_blocking_far_call_to_a_full_ring_waits_for_a_slot_then_its_reply)
        {
            uint32_t far = KCAP_INVALID;
            g_caller = seat_caller(1, PRIO_RECEIVER, &far);
            fill_outbound(far);
            ASSERT_FALSE(HasFailure());
            poison_cbuf();
            uint32_t const head = outbound_ring().head.v[OUTBOUND.index].load();
            uint16_t const seq = g_caller->call_seq;
            uint32_t const refused = amp::counts(ME).send_refused.load();
            wake_next_park(peer_frees_a_slot_then_replies);

            int32_t const rc = call(far);

            EXPECT_EQ(rc, static_cast<int32_t>(REPLY_LEN));
            EXPECT_TRUE(cbuf_holds(g_cbuf, REPLY_FILL, REPLY_LEN));
            EXPECT_EQ(outbound_ring().head.v[OUTBOUND.index].load(), head + 1u);
            EXPECT_EQ(g_caller->call_seq, static_cast<uint16_t>(seq + 1u))
                << "the wait for a slot spent a sequence";
            EXPECT_EQ(g_caller->far_hold, 0u);
            EXPECT_EQ(amp::counts(ME).send_refused.load(), refused)
                << "a wait for a slot was counted as a refusal";
        }

        TEST_F(AmpReplyHold, a_far_call_whose_deadline_passes_waiting_for_a_slot_publishes_nothing)
        {
            uint32_t far = KCAP_INVALID;
            g_caller = seat_caller(1, PRIO_RECEIVER, &far);
            fill_outbound(far);
            ASSERT_FALSE(HasFailure());
            poison_cbuf();
            uint32_t const head = outbound_ring().head.v[OUTBOUND.index].load();
            uint16_t const seq = g_caller->call_seq;
            uint32_t const sent = amp::counts(ME).sent.load();
            wake_next_park(deadline_passes);

            int32_t const rc = call(far);

            EXPECT_EQ(rc, -KOS_ETIMEDOUT);
            EXPECT_EQ(outbound_ring().head.v[OUTBOUND.index].load(), head);
            EXPECT_EQ(amp::counts(ME).sent.load(), sent);
            EXPECT_EQ(g_caller->call_seq, seq);
            EXPECT_EQ(g_caller->far_hold, 0u);
            EXPECT_TRUE(cbuf_holds(g_cbuf, REQUEST_FILL, CALL_LEN));
        }

        // A port the peer never takes a send on fills its own share of the call ring and no more:
        // a call to another of the peer's ports is still published, and its reply lands.
        TEST_F(AmpReplyHold, a_port_the_peer_takes_nothing_on_leaves_its_other_ports_their_slots)
        {
            uint32_t far = KCAP_INVALID;
            g_caller = seat_caller(1, PRIO_RECEIVER, &far);
            uint32_t echo = KCAP_INVALID;
            {
                IrqLock lock;
                ASSERT_EQ(amp_endpoint_mint(g_caller, PEER, amp::PORT_ECHO, CAP_SIGNAL, &echo), 0);
            }
            uint32_t sent = 0;
            while (sent <= amp::RING_SLOTS
                   and send_far(far, DEADLINE_POLL) == static_cast<int32_t>(CALL_LEN))
            {
                sent++;
            }
            ASSERT_GE(sent, 1u);
            EXPECT_EQ(send_far(far, DEADLINE_POLL), -KOS_EAGAIN) << "the port's share has no end";

            ASSERT_EQ(send_far(echo, DEADLINE_POLL), static_cast<int32_t>(CALL_LEN))
                << "a send to another port met the slots " << sent << " sends hold";
            poison_cbuf();
            g_call_lane = OUTBOUND_ECHO;
            wake_next_park(reply_arrives);
            EXPECT_EQ(call(echo), static_cast<int32_t>(REPLY_LEN));
            EXPECT_TRUE(cbuf_holds(g_cbuf, REPLY_FILL, REPLY_LEN));
            EXPECT_EQ(g_caller->far_hold, 0u);
        }

        // Killed while it waits, the sender leaves the ring as it found it, and the slot the peer
        // frees afterwards takes the next publication.
        TEST_F(AmpReplyHold, a_far_sender_killed_while_waiting_for_a_slot_leaves_the_ring_intact)
        {
            uint32_t far = KCAP_INVALID;
            g_caller = seat_caller(1, PRIO_RECEIVER, &far);
            fill_outbound(far);
            ASSERT_FALSE(HasFailure());
            uint32_t const head = outbound_ring().head.v[OUTBOUND.index].load();
            uint32_t const sent = amp::counts(ME).sent.load();
            wake_next_park(killed_while_waiting);

            int32_t const rc = send_far(far, KOS_LIMIT_NONE);

            EXPECT_EQ(rc, -KOS_ECANCELED);
            EXPECT_EQ(g_caller->wait_kind, WAIT_NONE);
            EXPECT_EQ(outbound_ring().head.v[OUTBOUND.index].load(), head);
            EXPECT_EQ(amp::counts(ME).sent.load(), sent);

            peer_frees_a_slot(g_caller);
            EXPECT_EQ(g_caller->state, ThreadState::RUNNING);
            EXPECT_EQ(outbound_used(), OUTBOUND.slots - 1u);
            IrqLock lock;
            EXPECT_EQ(amp::send(PEER, PORT_FAR, amp::REPLY_TAG_NONE, KCAP_BADGE_NONE, g_cbuf,
                                CALL_LEN),
                      amp::Sent::OK);
            EXPECT_EQ(outbound_used(), OUTBOUND.slots);
        }

        // --- the endpoint a waiting sender named goes away ---------------------------------
        // Nothing pins the far endpoint while its sender waits: its capability is closed and
        // its slot reused for a far endpoint toward another node, whose ring is full.
        namespace
        {
            uint32_t g_far_closed = KCAP_INVALID;
            Thread* g_other_holder = nullptr;
            Endpoint* g_far_before = nullptr;
            Endpoint* g_far_reused = nullptr;

            Endpoint* resolve(Thread* t, uint32_t cap)
            {
                IrqLock lock;
                int err = 0;
                return static_cast<Endpoint*>(
                    cap_resolve_e(t, cap, CapType::CAP_ENDPOINT, CAP_SIGNAL, &err));
            }

            void close_and_reuse_the_endpoint(Thread* parked)
            {
                {
                    IrqLock lock;
                    Endpoint* filler = nullptr;
                    while (endpoint_slot_claim(nullptr, &filler) >= 0)
                    {
                    }
                    EXPECT_EQ(handle_close(parked, g_far_closed, lock), 0);
                    uint32_t reused = KCAP_INVALID;
                    EXPECT_EQ(amp_endpoint_mint(g_other_holder, OTHER, PORT_OTHER, CAP_SIGNAL,
                                                &reused),
                              0);
                    int err = 0;
                    g_far_reused = static_cast<Endpoint*>(cap_resolve_e(
                        g_other_holder, reused, CapType::CAP_ENDPOINT, CAP_SIGNAL, &err));
                }
                fill_other();
            }

            void reused_then_peer_frees_a_slot(Thread* parked)
            {
                close_and_reuse_the_endpoint(parked);
                peer_frees_a_slot(parked);
            }

            void reused_then_other_frees_a_slot(Thread* parked)
            {
                close_and_reuse_the_endpoint(parked);
                other_frees_a_slot();
                EXPECT_EQ(parked->state, ThreadState::BLOCKED)
                    << "a slot of the reused endpoint's ring woke the sender";
                peer_frees_a_slot(parked);
            }

            Thread* seat_sender_and_holder(uint32_t* out_far)
            {
                g_other_holder = seat_pool(2, PRIO_BELOW);
                attach_caps(g_other_holder, KICKOS_SEAM_CAP_WIDTH);
                Thread* const c = seat_caller(1, PRIO_RECEIVER, out_far);
                g_far_closed = *out_far;
                g_far_before = resolve(c, *out_far);
                g_far_reused = nullptr;
                return c;
            }
        }

        TEST_F(AmpReplyHold, a_waiting_sender_whose_endpoint_was_reused_wakes_on_its_own_node)
        {
            uint32_t far = KCAP_INVALID;
            g_caller = seat_sender_and_holder(&far);
            fill_outbound(far);
            ASSERT_FALSE(HasFailure());
            uint32_t const head = outbound_ring().head.v[OUTBOUND.index].load();
            wake_next_park(reused_then_peer_frees_a_slot);

            int32_t const rc = send_far(far, KOS_LIMIT_NONE);

            ASSERT_EQ(g_far_reused, g_far_before) << "the mint took another slot";
            EXPECT_EQ(rc, -KOS_EBADF) << "the sender stayed parked on the reused slot's ring";
            EXPECT_EQ(g_caller->wait_kind, WAIT_NONE);
            EXPECT_EQ(outbound_ring().head.v[OUTBOUND.index].load(), head);
            EXPECT_EQ(other_ring().head.v[OTHER_LANE.index].load()
                          - other_ring().tail.v[OTHER_LANE.index].load(),
                      OTHER_LANE.slots);
        }

        TEST_F(AmpReplyHold, a_waiting_sender_whose_endpoint_was_reused_ignores_the_new_node)
        {
            uint32_t far = KCAP_INVALID;
            g_caller = seat_sender_and_holder(&far);
            fill_outbound(far);
            ASSERT_FALSE(HasFailure());
            wake_next_park(reused_then_other_frees_a_slot);

            int32_t const rc = send_far(far, KOS_LIMIT_NONE);

            ASSERT_EQ(g_far_reused, g_far_before) << "the mint took another slot";
            EXPECT_EQ(rc, -KOS_EBADF);
            EXPECT_EQ(g_caller->wait_kind, WAIT_NONE);
        }

        // --- many senders waiting ----------------------------------------------------------
        namespace
        {
            constexpr uint8_t PRIO_RUNNING = 30;

            // A sender parked for a slot of the full lane `lane` of the ring toward `node`, as
            // far_send_or_park leaves it, on a thread that is not running.
            Thread* far_waiter(int slot, uint8_t prio, uint32_t node, amp::Lane const& lane)
            {
                Thread* const t = seat_pool(slot, prio);
                IrqLock lock;
                uint32_t tail = 0;
                EXPECT_EQ(amp::call_ring_room(node, lane.index, &tail), 0u) << "node " << node;
                testfix::seat_blocked(t);
                policy_on_remove(t);
                t->wait_queue = nullptr;
                t->wait_kind = WAIT_EP_FAR_SEND;
                t->wait_obj = nullptr;
                t->wait_result = WAIT_RESULT_POISON;
                endpoint_far_send_seat(t, node, lane.index, tail);
                return t;
            }

            Thread* run_above_the_waiters()
            {
                Thread* const t = seat_pool(0, PRIO_RUNNING);
                IrqLock lock;
                sched::reschedule(nullptr, lock);
                return t;
            }

            void fill_raw(amp::Ring& r, amp::Lane const& lane)
            {
                r.head.v[lane.index].store(r.tail.v[lane.index].load() + lane.slots);
            }

            void give_back(amp::Ring& r, amp::Lane const& lane, uint32_t n)
            {
                r.tail.v[lane.index].store(r.tail.v[lane.index].load() + n);
            }

            bool slot_woken(Thread const* t)
            {
                return t->state == ThreadState::READY and t->wait_kind == WAIT_NONE
                       and t->wait_result == 0;
            }

            bool still_waiting(Thread const* t)
            {
                return t->state == ThreadState::BLOCKED and t->wait_kind == WAIT_EP_FAR_SEND;
            }
        }

        TEST_F(AmpHold, a_doorbell_wakes_as_many_senders_as_slots_came_back_by_priority_then_park)
        {
            run_above_the_waiters();
            fill_raw(outbound_ring(), OUTBOUND);
            Thread* const first = far_waiter(1, PRIO_BELOW, PEER, OUTBOUND);
            Thread* const high = far_waiter(2, PRIO_RECEIVER, PEER, OUTBOUND);
            Thread* const second = far_waiter(3, PRIO_BELOW, PEER, OUTBOUND);
            ASSERT_EQ(endpoint_far_send_waiting(PEER, OUTBOUND.index), 3u);

            give_back(outbound_ring(), OUTBOUND, 2u);
            doorbell();

            EXPECT_TRUE(slot_woken(high));
            EXPECT_TRUE(slot_woken(first));
            EXPECT_TRUE(still_waiting(second)) << "more senders woke than slots came back";
            EXPECT_EQ(endpoint_far_send_waiting(PEER, OUTBOUND.index), 1u);

            doorbell();
            EXPECT_TRUE(still_waiting(second)) << "a doorbell that gave nothing back woke one";
            EXPECT_EQ(endpoint_far_send_waiting(PEER, OUTBOUND.index), 1u);
        }

        TEST_F(AmpHold, a_partial_wake_keeps_each_nodes_waiters_apart)
        {
            run_above_the_waiters();
            fill_raw(outbound_ring(), OUTBOUND);
            fill_raw(other_ring(), OTHER_LANE);
            Thread* const peer_first = far_waiter(1, PRIO_BELOW, PEER, OUTBOUND);
            Thread* const other = far_waiter(2, PRIO_BELOW, OTHER, OTHER_LANE);
            Thread* const peer_second = far_waiter(3, PRIO_BELOW, PEER, OUTBOUND);

            give_back(outbound_ring(), OUTBOUND, 1u);
            doorbell();
            EXPECT_TRUE(slot_woken(peer_first));
            EXPECT_TRUE(still_waiting(other));
            EXPECT_TRUE(still_waiting(peer_second));
            EXPECT_EQ(endpoint_far_send_waiting(PEER, OUTBOUND.index), 1u);
            EXPECT_EQ(endpoint_far_send_waiting(OTHER, OTHER_LANE.index), 1u);

            give_back(other_ring(), OTHER_LANE, 1u);
            doorbell();
            EXPECT_TRUE(slot_woken(other));
            EXPECT_TRUE(still_waiting(peer_second));
            EXPECT_EQ(endpoint_far_send_waiting(PEER, OUTBOUND.index), 1u);
            EXPECT_EQ(endpoint_far_send_waiting(OTHER, OTHER_LANE.index), 0u);

            give_back(outbound_ring(), OUTBOUND, 1u);
            doorbell();
            EXPECT_TRUE(slot_woken(peer_second));
            EXPECT_EQ(endpoint_far_send_waiting(PEER, OUTBOUND.index), 0u);
            EXPECT_EQ(endpoint_far_send_waiting(OTHER, OTHER_LANE.index), 0u);
        }

        // A slot one port's lane gives back wakes that lane's waiters and no other's, the two
        // ports leading to the same node.
        TEST_F(AmpHold, a_partial_wake_keeps_each_lanes_waiters_apart)
        {
            run_above_the_waiters();
            fill_raw(outbound_ring(), OUTBOUND);
            fill_raw(outbound_ring(), OUTBOUND_ECHO);
            Thread* const port = far_waiter(1, PRIO_BELOW, PEER, OUTBOUND);
            Thread* const echo = far_waiter(2, PRIO_RECEIVER, PEER, OUTBOUND_ECHO);

            give_back(outbound_ring(), OUTBOUND, 1u);
            doorbell();

            EXPECT_TRUE(slot_woken(port));
            EXPECT_TRUE(still_waiting(echo)) << "a slot of another port's lane woke the sender";
            EXPECT_EQ(endpoint_far_send_waiting(PEER, OUTBOUND.index), 0u);
            EXPECT_EQ(endpoint_far_send_waiting(PEER, OUTBOUND_ECHO.index), 1u);

            give_back(outbound_ring(), OUTBOUND_ECHO, 1u);
            doorbell();
            EXPECT_TRUE(slot_woken(echo));
            EXPECT_EQ(endpoint_far_send_waiting(PEER, OUTBOUND_ECHO.index), 0u);
        }

        TEST_F(AmpHold, a_cancel_or_a_timeout_leaves_no_far_slot_wait_behind)
        {
            Thread* const running = run_above_the_waiters();
            fill_raw(outbound_ring(), OUTBOUND);
            Thread* const timed = far_waiter(1, PRIO_BELOW, PEER, OUTBOUND);
            Thread* const killed = far_waiter(2, PRIO_BELOW, PEER, OUTBOUND);
            ASSERT_EQ(endpoint_far_send_waiting(PEER, OUTBOUND.index), 2u);

            thread_abort_park(timed, -KOS_ETIMEDOUT, kickos::IrqLock());
            thread_cancel_kind(killed, CANCEL_KILL, kickos::IrqLock());
            EXPECT_EQ(endpoint_far_send_waiting(PEER, OUTBOUND.index), 0u);
            EXPECT_EQ(timed->wait_kind, WAIT_NONE);
            EXPECT_EQ(timed->wait_result, -KOS_ETIMEDOUT);
            EXPECT_EQ(killed->wait_kind, WAIT_NONE);
            EXPECT_EQ(killed->wait_result, -KOS_ECANCELED);

            give_back(outbound_ring(), OUTBOUND, 1u);
            doorbell();
            EXPECT_EQ(timed->wait_result, -KOS_ETIMEDOUT) << "the wake reached a sender gone";
            EXPECT_EQ(killed->wait_result, -KOS_ECANCELED) << "the wake reached a sender gone";
            EXPECT_EQ(sched::current(), running);
        }

        // A woken sender that dies before it publishes hands its slot to the next waiter.
        TEST_F(AmpHold, a_woken_sender_slain_before_it_publishes_hands_its_slot_on)
        {
            Thread* const running = run_above_the_waiters();
            fill_raw(outbound_ring(), OUTBOUND);
            Thread* const woken = far_waiter(1, PRIO_RECEIVER, PEER, OUTBOUND);
            Thread* const next = far_waiter(2, PRIO_BELOW, PEER, OUTBOUND);
            give_back(outbound_ring(), OUTBOUND, 1u);
            doorbell();
            ASSERT_TRUE(slot_woken(woken));
            ASSERT_TRUE(still_waiting(next));
            {
                IrqLock lock;
                thread_cancel_kind(woken, CANCEL_SLAY, lock);
                sched::set_prio(running, PRIO_BELOW, lock);
                sched::reschedule(nullptr, lock);
            }
            ASSERT_EQ(sched::current(), woken);
            run_noreturn(slay_exit);

            EXPECT_TRUE(slot_woken(next)) << "the slot the slain sender was woken for is lost";
            EXPECT_EQ(endpoint_far_send_waiting(PEER, OUTBOUND.index), 0u);
        }

        namespace
        {
            void parked_again_until_a_slot_comes_back(Thread* parked)
            {
                EXPECT_EQ(parked->wait_kind, WAIT_EP_FAR_SEND) << "the sender lost its slot";
                EXPECT_EQ(endpoint_far_send_waiting(PEER, OUTBOUND.index), 1u);
                kickos_amp_node_service();
                EXPECT_EQ(parked->state, ThreadState::BLOCKED)
                    << "a doorbell that gave nothing back woke the sender again";
                peer_frees_a_slot(parked);
            }

            void slot_taken_before_the_woken_sender_runs(Thread* parked)
            {
                peer_frees_a_slot(parked);
                EXPECT_EQ(parked->wait_kind, WAIT_NONE);
                IrqLock lock;
                EXPECT_EQ(amp::send(PEER, PORT_FAR, amp::REPLY_TAG_NONE, KCAP_BADGE_NONE, g_cbuf,
                                    CALL_LEN),
                          amp::Sent::OK);
                wake_next_park(parked_again_until_a_slot_comes_back);
            }
        }

        // A woken sender whose slot a fresh publication took parks again, and only a slot given
        // back wakes it.
        TEST_F(AmpReplyHold, a_woken_sender_that_lost_its_slot_parks_again_until_the_next)
        {
            uint32_t far = KCAP_INVALID;
            g_caller = seat_caller(1, PRIO_RECEIVER, &far);
            fill_outbound(far);
            ASSERT_FALSE(HasFailure());
            uint32_t const head = outbound_ring().head.v[OUTBOUND.index].load();
            wake_next_park(slot_taken_before_the_woken_sender_runs);

            int32_t const rc = send_far(far, KOS_LIMIT_NONE);

            EXPECT_EQ(rc, static_cast<int32_t>(CALL_LEN));
            EXPECT_EQ(outbound_ring().head.v[OUTBOUND.index].load(), head + 2u);
            EXPECT_EQ(endpoint_far_send_waiting(PEER, OUTBOUND.index), 0u);
        }

        TEST_F(AmpReplyHold, a_far_sender_that_left_its_wait_is_no_longer_counted)
        {
            uint32_t far = KCAP_INVALID;
            g_caller = seat_caller(1, PRIO_RECEIVER, &far);
            fill_outbound(far);
            ASSERT_FALSE(HasFailure());
            wake_next_park(deadline_passes);
            EXPECT_EQ(send_far(far, KOS_LIMIT_NONE), -KOS_ETIMEDOUT);
            EXPECT_EQ(endpoint_far_send_waiting(PEER, OUTBOUND.index), 0u);
            wake_next_park(killed_while_waiting);
            EXPECT_EQ(call(far), -KOS_ECANCELED);
            EXPECT_EQ(endpoint_far_send_waiting(PEER, OUTBOUND.index), 0u);
        }

        // --- the reply guard ---------------------------------------------------------------
        // Each case ends on the caller's own tag over the same staging, the control.
        namespace
        {
            void expect_still_parked(Thread const* c)
            {
                EXPECT_EQ(c->state, ThreadState::BLOCKED) << "a hostile reply woke the caller";
                EXPECT_EQ(c->far_hold, 0u) << "a hostile reply was handed to the caller";
                EXPECT_EQ(c->call_state, CALL_REPLY_WAIT);
                EXPECT_EQ(c->wait_kind, WAIT_EP_FAR_REPLY);
            }

            void hostile_reply_is_dropped(Thread const* c, amp::ReplyTag const& tag)
            {
                uint32_t const drops = amp::counts(ME).reply_drop.load();
                peer_replies(tag);
                uint32_t const tail_before = reply_ring().tail.v[0].load();
                doorbell();
                EXPECT_EQ(amp::counts(ME).reply_drop.load(), drops + 1u)
                    << "the refusal was not counted";
                EXPECT_EQ(reply_ring().tail.v[0].load(), tail_before + 1u)
                    << "a reply nobody lands was left holding the ring";
                expect_still_parked(c);
            }

            void own_reply_completes(Thread const* c, amp::ReplyTag const& tag)
            {
                uint32_t const drops = amp::counts(ME).reply_drop.load();
                peer_replies(tag);
                uint32_t const tail_before = reply_ring().tail.v[0].load();
                doorbell();
                EXPECT_EQ(c->state, ThreadState::READY) << "the control did not complete";
                EXPECT_NE(c->far_hold, 0u);
                EXPECT_EQ(c->wait_result, static_cast<intptr_t>(REPLY_LEN));
                EXPECT_EQ(amp::counts(ME).reply_drop.load(), drops);
                EXPECT_EQ(reply_ring().tail.v[0].load(), tail_before);
            }
        }

        TEST_F(AmpReplyHold, a_reply_naming_a_thread_not_parked_in_a_call_is_dropped)
        {
            Thread* above = nullptr;
            amp::ReplyTag tag = {};
            Thread* const c = parked_behind(&above, &tag);
            ASSERT_FALSE(HasFailure());
            amp::ReplyTag const running = {
                kernel().threads.handle_for(kernel().threads.index_of(above)),
                amp::reply_seq(above->call_seq)};

            hostile_reply_is_dropped(c, running);
            EXPECT_EQ(above->state, ThreadState::RUNNING);
            EXPECT_EQ(above->far_hold, 0u);

            own_reply_completes(c, tag);
        }

        TEST_F(AmpReplyHold, a_reply_from_a_node_the_caller_did_not_call_is_refused)
        {
            Thread* above = nullptr;
            amp::ReplyTag tag = {};
            Thread* const c = parked_behind(&above, &tag);
            ASSERT_FALSE(HasFailure());
            constexpr uint32_t HOLD = 3u;
            uint32_t const strangers[] = {ME, PEER + 1u};
            for (uint32_t from : strangers)
            {
                IrqLock lock;
                EXPECT_FALSE(endpoint_far_reply_deliver(from, tag, HOLD, REPLY_LEN, false, lock))
                    << "node " << from;
            }
            expect_still_parked(c);

            {
                IrqLock lock;
                EXPECT_TRUE(endpoint_far_reply_deliver(PEER, tag, HOLD, REPLY_LEN, false, lock))
                    << "the control does not complete";
            }
            EXPECT_EQ(c->state, ThreadState::READY);
            EXPECT_EQ(c->far_hold, HOLD + 1u);
        }

        TEST_F(AmpReplyHold, a_reply_one_sequence_ahead_of_the_call_is_dropped)
        {
            Thread* above = nullptr;
            amp::ReplyTag tag = {};
            Thread* const c = parked_behind(&above, &tag);
            ASSERT_FALSE(HasFailure());

            hostile_reply_is_dropped(c, {tag.thread, amp::reply_seq(tag.seq + 1u)});

            own_reply_completes(c, tag);
        }

        // The low byte left standing: a guard validating that byte alone takes it.
        TEST_F(AmpReplyHold, a_reply_whose_sequence_differs_above_the_low_byte_is_dropped)
        {
            Thread* above = nullptr;
            amp::ReplyTag tag = {};
            Thread* const c = parked_behind(&above, &tag);
            ASSERT_FALSE(HasFailure());

            hostile_reply_is_dropped(c, {tag.thread, amp::reply_seq(tag.seq + 0x100u)});

            own_reply_completes(c, tag);
        }

        // --- the far mint ----------------------------------------------------------------
        TEST_F(AmpHold, the_far_mint_refuses_the_reply_port_and_mints_a_service)
        {
            Thread* const c = seat_pool(1, PRIO_RECEIVER);
            attach_caps(c, KICKOS_SEAM_CAP_WIDTH);
            IrqLock lock;
            uint32_t cap = 0u;
            EXPECT_EQ(amp_endpoint_mint(c, PEER, amp::PORT_REPLY, CAP_SIGNAL, &cap), -KOS_EINVAL);
            EXPECT_EQ(cap, KCAP_INVALID);
            EXPECT_EQ(amp_endpoint_mint(c, ME, PORT_SERVED, CAP_SIGNAL, &cap), -KOS_EINVAL)
                << "a far endpoint was minted on this node itself";
            EXPECT_EQ(amp_endpoint_mint(c, PEER, PORT_UNMINTED, CAP_SIGNAL, &cap), -KOS_EINVAL);

            EXPECT_EQ(amp_endpoint_mint(c, PEER, amp::PORT_ECHO, CAP_SIGNAL, &cap), 0);
            EXPECT_NE(cap, KCAP_INVALID);
            EXPECT_EQ(amp_endpoint_mint(c, PEER, PORT_FAR, CAP_SIGNAL, &cap), 0);
            EXPECT_NE(cap, KCAP_INVALID);
        }

        // The pool full but for the far endpoint's slot, so the create after its close lands
        // there: it must not carry the far route.
        TEST_F(AmpHold, a_far_endpoint_slot_freed_and_created_again_is_local)
        {
            uint32_t far = KCAP_INVALID;
            Thread* const c = seat_caller(1, PRIO_RECEIVER, &far);
            ASSERT_EQ(sched::current(), c);
            Endpoint* far_ep = nullptr;
            {
                IrqLock lock;
                int err = 0;
                far_ep = static_cast<Endpoint*>(
                    cap_resolve_e(c, far, CapType::CAP_ENDPOINT, CAP_SIGNAL, &err));
                ASSERT_NE(far_ep, nullptr);
                ASSERT_TRUE(endpoint_is_far(far_ep));
                Endpoint* filler = nullptr;
                while (endpoint_slot_claim(nullptr, &filler) >= 0)
                {
                }
                ASSERT_EQ(handle_close(c, far, lock), 0);
            }
            uint32_t local = KCAP_INVALID;
            ASSERT_EQ(endpoint_create(&local), 0);
            Endpoint* reused = nullptr;
            {
                IrqLock lock;
                int err = 0;
                reused = static_cast<Endpoint*>(
                    cap_resolve_e(c, local, CapType::CAP_ENDPOINT, CAP_SIGNAL, &err));
            }
            ASSERT_EQ(reused, far_ep) << "the create took another slot, so nothing was reused";
            EXPECT_FALSE(endpoint_is_far(reused)) << "the reused slot still carries the far route";

            uint32_t const out_head = outbound_ring().head.v[OUTBOUND.index].load();
            uint32_t const sent = amp::counts(ME).sent.load();
            EXPECT_EQ(endpoint_send(local, reinterpret_cast<uintptr_t>(g_cbuf), CALL_LEN, 0u),
                      -KOS_EAGAIN);
            EXPECT_EQ(outbound_ring().head.v[OUTBOUND.index].load(), out_head)
                << "the send crossed to the peer";
            EXPECT_EQ(amp::counts(ME).sent.load(), sent);
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
                attach_caps(g_fs.c, KICKOS_SEAM_CAP_WIDTH);
                g_fs.ep = endpoint();
                g_fs.ep->recv_holders = 1;
                int const obj =
                    kernel().endpoints.handle_for(kernel().endpoints.index_of(g_fs.ep));
                ASSERT_EQ(cap_install(g_fs.c, obj, CapType::CAP_ENDPOINT, CAP_SIGNAL, &g_fs.cap),
                          0);
                g_fs.w = spawn(0, PRIO_RECEIVER);
                attach_caps(g_fs.w, KICKOS_SEAM_CAP_WIDTH);
                {
                    IrqLock lock;
                    testfix::seat_blocked(g_fs.w);
                    policy_on_remove(g_fs.w);
                    g_fs.w->wait_queue = &g_fs.ep->recv_waiters;
                    g_fs.w->wait_kind = WAIT_EP_RECV;
                    g_fs.w->wait_obj = g_fs.ep;
                    g_fs.w->ipc.buf = reinterpret_cast<uintptr_t>(g_fs.recv_buf);
                    g_fs.w->ipc.len = sizeof(g_fs.recv_buf);
                    g_fs.w->ipc.badge_out = reinterpret_cast<uintptr_t>(&g_fs.info);
                    g_fs.ep->recv_waiters.push_back(&g_fs.w->link);
                    sched::reschedule(nullptr, lock);
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
            uint32_t const out_head = outbound_ring().head.v[OUTBOUND.index].load();
            uint16_t const seq = g_fs.c->call_seq;

            EXPECT_EQ(kickos_ipc_fastpath(g_fs.args), nullptr);
            EXPECT_EQ(g_fs.c->call_seq, seq);
            EXPECT_EQ(g_fs.c->state, ThreadState::RUNNING);
            EXPECT_EQ(g_fs.c->call_frame_parked, 0u);
            EXPECT_EQ(g_fs.w->state, ThreadState::BLOCKED);
            EXPECT_EQ(outbound_ring().head.v[OUTBOUND.index].load(), out_head);

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
            uint32_t const tail_before = reply_ring().tail.v[0].load();
            doorbell();

            EXPECT_EQ(c->far_hold, 0u) << "a caller with no continuation was handed a hold";
            EXPECT_EQ(c->state, ThreadState::BLOCKED);
            EXPECT_EQ(c->call_state, CALL_REPLY_WAIT);
            EXPECT_EQ(amp::counts(ME).reply_drop.load(), drops + 1u);
            EXPECT_EQ(reply_ring().tail.v[0].load(), tail_before + 1u);
        }

        // A local port's binding names its endpoint by slot index, so closing the capability
        // leaves the slot live and bound, and no later claim is handed it.
        TEST_F(AmpHold, closing_a_local_port_capability_keeps_its_slot_from_every_later_claim)
        {
            uint32_t ep = KCAP_INVALID;
            Thread* const r = seat_receiver(0, PRIO_RECEIVER, &ep);
            uint16_t const bound = amp::port_endpoint(PORT_SERVED);
            ASSERT_NE(bound, amp::EP_BOUND_NONE);
            IrqLock lock;
            ASSERT_EQ(handle_close(r, ep, lock), 0);
            EXPECT_EQ(kernel().endpoints.refs()[bound], 1u);
            EXPECT_EQ(amp::port_endpoint(PORT_SERVED), bound);
            int claimed = 0;
            Endpoint* filler = nullptr;
            int at = endpoint_slot_claim(nullptr, &filler);
            while (kernel().endpoints.at(at) != nullptr)
            {
                EXPECT_NE(at, static_cast<int>(bound));
                claimed++;
                at = endpoint_slot_claim(nullptr, &filler);
            }
            EXPECT_GT(claimed, 0);
        }
    }
}
