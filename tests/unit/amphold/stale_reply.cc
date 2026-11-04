// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A reply capability names its caller by a thread handle carrying 16 bits of the slot's
// generation and an 8-bit call sequence, and a far one names a record by a token carrying 16
// bits of the record's. The entry keeps the whole 32-bit generation: once the slot has been
// reused 65536 times, a kept reply cap whose handle and sequence match a new occupant parked in
// a call is still refused, and so is one whose record has died and seated again as often. A
// reply to a slot's or a record's first incarnation, at generation 0, is refused once it ends.

#include <string.h>

#include <kickos/ampwindow.h>
#include <kickos/cap.h>
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

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            constexpr uint32_t WRAP = 1u << 16;
            constexpr uint32_t PEER = 1;
            uint8_t g_buf[16];

            class StaleReply : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    amphold::window_cold();
                }
            };

            // `t` parked in a call, as endpoint_call leaves its caller once a receiver took it.
            void park_in_call(Thread* t)
            {
                t->ipc.buf = reinterpret_cast<uintptr_t>(g_buf);
                t->call_rx_cap = sizeof(g_buf);
                t->call_state = CALL_REPLY_WAIT;
                t->wait_kind = WAIT_EP_REPLY;
                t->wait_result = WAIT_RESULT_POISON;
            }
        }

        TEST_F(StaleReply, a_reply_kept_across_a_generation_wrap_of_its_callers_slot_is_refused)
        {
            Thread* const server = seat_pool(1, 20);
            attach_caps(server, KICKOS_SEAM_CAP_WIDTH);
            Thread* const caller = seat_pool(2, 10);
            int const slot = kernel().threads.index_of(caller);
            {
                IrqLock lock;
                sched::reschedule(nullptr, lock);
            }
            ASSERT_EQ(sched::current(), server);
            uint32_t kept = KCAP_INVALID;
            {
                IrqLock lock;
                testfix::seat_blocked(caller);
                policy_on_remove(caller);
                park_in_call(caller);
                ASSERT_EQ(cap_install_reply(server, caller, &kept), 0);
            }
            // The call ends without a reply, and the slot is reclaimed until the handle's 16
            // bits of generation come round; its new occupant is parked in a call at the same
            // sequence, taken by the same server.
            for (uint32_t i = 0; i < WRAP; i++)
            {
                kernel().threads.gen_step(slot);
            }
            {
                IrqLock lock;
                park_in_call(caller);
                reply_donor_park(server, caller);
                ASSERT_EQ(cap_reply_thread(cap_reply_handle(*cap_lookup(server, kept)), caller->call_seq,
                                           KCAP_REPLY_SEQ_MASK),
                          caller)
                    << "fixture: the handle and the sequence name the new occupant";
            }
            uint8_t const answer[2] = {1u, 2u};
            EXPECT_EQ(endpoint_reply(kept, reinterpret_cast<uintptr_t>(answer), sizeof(answer)),
                      -KOS_ESRCH)
                << "the kept reply answered the slot's new occupant";
            EXPECT_EQ(caller->wait_result, WAIT_RESULT_POISON);
            EXPECT_EQ(caller->state, ThreadState::BLOCKED);
        }

        TEST_F(StaleReply, a_far_reply_kept_across_a_generation_wrap_of_its_record_is_refused)
        {
            Thread* const server = seat_pool(1, 20);
            attach_caps(server, KICKOS_SEAM_CAP_WIDTH);
            {
                IrqLock lock;
                sched::reschedule(nullptr, lock);
            }
            amp::window_init();
            amp::ReplyTag const tag = {0x0A0B0C0Du, 0x1234u};
            uint32_t record = 0;
            uint32_t kept = KCAP_INVALID;
            {
                IrqLock lock;
                record = amp::inbound_seat(PEER, 0u, tag, KOS_BADGE_NONE);
                ASSERT_NE(record, amp::FAR_RECORD_NONE);
                ASSERT_EQ(cap_install_far_reply(server, record, &kept), 0);
                // The record dies and seats again until the token's 16 bits come round.
                for (uint32_t i = 0; i < WRAP; i++)
                {
                    amp::inbound_forget(record);
                    record = amp::inbound_seat(PEER, 0u, tag, KOS_BADGE_NONE);
                }
                ASSERT_EQ(ThreadPool::far_reply_record(cap_reply_handle(*cap_lookup(server, kept))),
                          record)
                    << "fixture: the kept token names the record's new occupant";
            }
            amp::Ring const& back = amp::ring_for(amp::Class::REPLY, PEER, 0u);
            uint32_t const head = back.head.v.load();
            uint8_t const answer[2] = {1u, 2u};
            EXPECT_EQ(endpoint_reply(kept, reinterpret_cast<uintptr_t>(answer), sizeof(answer)),
                      -KOS_ESRCH);
            EXPECT_EQ(back.head.v.load(), head) << "the kept reply answered a stranger's far call";
            EXPECT_NE(amp::inbound_at(record), nullptr) << "the stranger's record was released";
        }

        TEST_F(StaleReply, a_reply_to_a_slot_s_first_incarnation_is_not_live_once_the_slot_moved_on)
        {
            Thread* const server = seat_pool(1, 20);
            attach_caps(server, KICKOS_SEAM_CAP_WIDTH);
            Thread* const caller = seat_pool(2, 10);
            int const slot = kernel().threads.index_of(caller);
            ASSERT_EQ(kernel().threads.wide_gen(slot), 0u) << "fixture: not the slot's first incarnation";
            {
                IrqLock lock;
                sched::reschedule(nullptr, lock);
            }
            uint32_t kept = KCAP_INVALID;
            {
                IrqLock lock;
                testfix::seat_blocked(caller);
                policy_on_remove(caller);
                park_in_call(caller);
                ASSERT_EQ(cap_install_reply(server, caller, &kept), 0);
            }
            kernel().threads.gen_step(slot);
            IrqLock lock;
            CapEntry const* const e = cap_lookup(server, kept);
            ASSERT_NE(e, nullptr);
            EXPECT_FALSE(cap_obj_live(*e)) << "the reply still names its reclaimed caller";
            EXPECT_EQ(cap_obj(*e), -1);
        }

        TEST_F(StaleReply, a_far_reply_to_a_record_s_first_incarnation_is_refused_once_it_died)
        {
            Thread* const server = seat_pool(1, 20);
            attach_caps(server, KICKOS_SEAM_CAP_WIDTH);
            {
                IrqLock lock;
                sched::reschedule(nullptr, lock);
            }
            amp::window_init();
            amp::ReplyTag const tag = {0x0A0B0C0Du, 0x1234u};
            uint32_t kept = KCAP_INVALID;
            {
                IrqLock lock;
                uint32_t const record = amp::inbound_seat(PEER, 0u, tag, KOS_BADGE_NONE);
                ASSERT_NE(record, amp::FAR_RECORD_NONE);
                ASSERT_EQ(amp::inbound_generation(record), 0u) << "fixture: not the record's first incarnation";
                ASSERT_EQ(cap_install_far_reply(server, record, &kept), 0);
                amp::inbound_forget(record);
            }
            amp::Ring const& back = amp::ring_for(amp::Class::REPLY, PEER, 0u);
            uint32_t const head = back.head.v.load();
            uint8_t const answer[2] = {1u, 2u};
            EXPECT_EQ(endpoint_reply(kept, reinterpret_cast<uintptr_t>(answer), sizeof(answer)),
                      -KOS_ESRCH)
                << "a reply to a dead record reported a delivery";
            EXPECT_EQ(back.head.v.load(), head);
        }
    }
}
