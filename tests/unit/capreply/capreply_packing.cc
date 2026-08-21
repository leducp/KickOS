// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Round-trip gate for a CAP_REPLY entry's packing (kernel/include/kickos/cap.h): the parked
// caller's whole 32-bit generational THREAD handle in `obj`, its 8-bit call sequence split
// into the spare bits beside the type and the rights.
//
// It drives the header's own cap_reply_seq_seat / cap_reply_seq / cap_reply_handle, the same
// functions cap_install_reply and cap_reply_caller call, so the gate cannot pass against a
// mirror of the arithmetic that has drifted from the kernel's.

#include <ios>
#include <stdint.h>
#include <stdio.h>

#include <gtest/gtest.h>

#include <kickos/cap.h>

using kickos::cap_reply_handle;
using kickos::cap_reply_seq;
using kickos::cap_reply_seq_seat;
using kickos::CapEntry;
using kickos::CapRights;
using kickos::CapType;

namespace
{
    // The kernel's order: cap_install seats obj/type/rights, cap_install_reply then seats the
    // sequence.
    void mint(CapEntry* e, uint32_t thread_handle, uint8_t seq8)
    {
        e->obj = static_cast<int32_t>(thread_handle);
        e->type = static_cast<uint8_t>(CapType::CAP_REPLY);
        e->rights = 0;
        e->gen = 0;
        cap_reply_seq_seat(e, seq8);
    }
}

// Publishes the footprint in the run log; cap.h static_asserts the same two numbers.
TEST(CapReply, entry_footprint)
{
    printf("# sizeof(CapEntry)=%u alignof(CapEntry)=%u\n",
           static_cast<unsigned>(sizeof(CapEntry)), static_cast<unsigned>(alignof(CapEntry)));
    EXPECT_EQ(sizeof(CapEntry), 8u) << "CapEntry is still 8 bytes with the sequence relocated";
    EXPECT_EQ(alignof(CapEntry), 8u) << "and still 8-aligned";
}

// Every (index, generation, sequence) triple survives the entry intact: no byte of the
// handle may be traded away for the sequence.
TEST(CapReply, round_trip_over_the_whole_word)
{
    // Values straddling every edge of the packing: 255/256 is a byte edge in the index,
    // 0x8000 in the generation is where the handle word goes negative, 0xFFFF is the top of
    // the generation, and 0x1F/0x20 straddles the sequence's 5-bit low half. The index stops
    // at 65534: 0xFFFF is KCAP_RESERVED_INDEX and is never seated.
    uint32_t const indices[] = {0u, 1u, 254u, 255u, 256u, 257u, 4095u, 32767u, 32768u, 65534u};
    uint32_t const gens[] = {0u, 1u, 255u, 256u, 0x7FFFu, 0x8000u, 0xFFFFu};
    uint32_t const seqs[] = {0u, 1u, 0x1Fu, 0x20u, 0x7Fu, 0x80u, 0xFEu, 0xFFu};

    uint32_t checked = 0;
    uint32_t negative = 0;
    for (uint32_t i = 0; i < sizeof(indices) / sizeof(indices[0]); i++)
    {
        for (uint32_t g = 0; g < sizeof(gens) / sizeof(gens[0]); g++)
        {
            uint32_t const handle = (gens[g] << 16) | indices[i];
            for (uint32_t s = 0; s < sizeof(seqs) / sizeof(seqs[0]); s++)
            {
                CapEntry e = {};
                mint(&e, handle, static_cast<uint8_t>(seqs[s]));
                checked++;
                if (e.obj < 0)
                {
                    negative++;
                }
                ASSERT_EQ(cap_reply_handle(e), handle)
                    << std::hex << "handle 0x" << handle << " came back 0x"
                    << cap_reply_handle(e) << " (seq 0x" << seqs[s] << ")";
                ASSERT_EQ(cap_reply_seq(e), static_cast<uint8_t>(seqs[s]))
                    << std::hex << "seq 0x" << seqs[s] << " came back 0x"
                    << static_cast<uint32_t>(cap_reply_seq(e)) << " (handle 0x" << handle << ")";
                // A bitfield cannot bind to the const reference EXPECT_EQ takes.
                ASSERT_EQ(static_cast<uint32_t>(e.type),
                          static_cast<uint32_t>(CapType::CAP_REPLY))
                    << std::hex << "the sequence overwrote the type (handle 0x" << handle << ")";
                ASSERT_EQ(static_cast<uint32_t>(e.rights), 0u)
                    << std::hex << "the sequence set a rights bit 0x"
                    << static_cast<uint32_t>(e.rights) << " (handle 0x" << handle << ")";
            }
        }
    }
    printf("# %u (index, generation, sequence) triples round-trip, %u of them negative\n", checked,
           negative);
    EXPECT_GT(negative, 0u) << "the sample includes handles bit 31 makes negative";
}

// A thread index above 255 must not cost a bit of the generation, which is the cross-thread
// ABA counter.
TEST(CapReply, index_above_the_old_ceiling_keeps_the_full_generation)
{
    CapEntry e = {};
    uint32_t const handle = (0xFFFFu << 16) | 300u;
    mint(&e, handle, 0xA5u);
    EXPECT_EQ(cap_reply_handle(e), handle)
        << "thread index 300 with a fully aged generation survives the entry";
    EXPECT_EQ(cap_reply_handle(e) >> 16, 0xFFFFu) << "and all 16 generation bits come back, not 9";
    EXPECT_EQ(cap_reply_seq(e), 0xA5u) << "with its call sequence beside it";
}

// The sequence shares its two bytes with the type and the rights, so a sequence value
// landing on the CAP_TRANSFER bit would make a one-shot reply cap delegable.
TEST(CapReply, sequence_can_never_forge_a_right)
{
    for (uint32_t s = 0; s < 256u; s++)
    {
        CapEntry e = {};
        mint(&e, 1u, static_cast<uint8_t>(s));
        // A bitfield cannot bind to the const reference EXPECT_EQ takes.
        ASSERT_EQ(static_cast<uint32_t>(e.rights), 0u)
            << std::hex << "sequence 0x" << s << " leaked rights 0x"
            << static_cast<uint32_t>(e.rights);
        ASSERT_EQ(static_cast<uint32_t>(e.type), static_cast<uint32_t>(CapType::CAP_REPLY))
            << std::hex << "sequence 0x" << s << " corrupted the type";
    }
    // And the reverse: seating every right must leave the sequence alone.
    uint8_t const all = CapRights::CAP_WAIT | CapRights::CAP_SIGNAL | CapRights::CAP_TRANSFER;
    for (uint8_t r = 0; r <= all; r++)
    {
        CapEntry e = {};
        mint(&e, 1u, 0xFFu);
        e.rights = r;
        ASSERT_EQ(cap_reply_seq(e), 0xFFu)
            << std::hex << "rights 0x" << static_cast<uint32_t>(r) << " corrupted the sequence";
    }
    printf("# all 256 sequence values leave the type and the rights untouched\n");
}
