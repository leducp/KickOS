// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Round-trip gate for a CAP_REPLY entry's packing (kernel/include/kickos/cap.h): the parked
// caller's whole 32-bit generational THREAD handle in `obj`, its 8-bit call sequence in the
// spare bits beside the type and the rights.
//
// It drives the header's own cap_reply_seq_seat / cap_reply_seq / cap_reply_handle, the same
// functions cap_install_reply and cap_reply_caller call, so the gate cannot pass against a
// mirror of the arithmetic that has drifted from the kernel's.
//
// Host-only: no cap-table entry is observable from userspace, and the case that matters is a
// thread INDEX above 255, which needs more thread slots than any board in the fleet
// configures.

#include <stdint.h>
#include <stdio.h>

#include <kickos/cap.h>

using kickos::cap_reply_handle;
using kickos::cap_reply_seq;
using kickos::cap_reply_seq_seat;
using kickos::CapEntry;
using kickos::CapRights;
using kickos::CapType;

namespace
{
    int g_failures = 0;

    void check(bool ok, char const* what)
    {
        if (ok)
        {
            return;
        }
        printf("not ok - %s\n", what);
        g_failures++;
    }

    // What the kernel writes: cap_install seats obj/type/rights, cap_install_reply then seats
    // the sequence.
    void mint(CapEntry* e, uint32_t thread_handle, uint8_t seq8)
    {
        e->obj = static_cast<int32_t>(thread_handle);
        e->type = static_cast<uint8_t>(CapType::CAP_REPLY);
        e->rights = 0;
        e->gen = 0;
        cap_reply_seq_seat(e, seq8);
    }

    // Every (index, generation, sequence) triple survives the entry intact: no byte of the
    // handle may be traded away for the sequence.
    void case_round_trip_over_the_whole_word()
    {
        // Values straddling every byte boundary of the handle word: 255/256 is a byte edge in
        // the index, 0x8000 is where the handle goes negative, and 0xFFFF is the top of both
        // fields.
        uint32_t const indices[] = {0u, 1u, 254u, 255u, 256u, 257u, 4095u, 32767u, 32768u,
                                    65534u};
        uint32_t const gens[] = {0u, 1u, 255u, 256u, 0x7FFFu, 0x8000u, 0xFFFFu};
        uint32_t const seqs[] = {0u, 1u, 0x1Fu, 0x20u, 0x7Fu, 0x80u, 0xFEu, 0xFFu};

        unsigned checked = 0;
        unsigned negative = 0;
        for (unsigned i = 0; i < sizeof(indices) / sizeof(indices[0]); i++)
        {
            for (unsigned g = 0; g < sizeof(gens) / sizeof(gens[0]); g++)
            {
                uint32_t const handle = (gens[g] << 16) | indices[i];
                for (unsigned s = 0; s < sizeof(seqs) / sizeof(seqs[0]); s++)
                {
                    CapEntry e = {};
                    mint(&e, handle, static_cast<uint8_t>(seqs[s]));
                    checked++;
                    if (e.obj < 0)
                    {
                        negative++;
                    }
                    if (cap_reply_handle(e) != handle)
                    {
                        printf("not ok - handle 0x%08x came back 0x%08x (seq 0x%02x)\n", handle,
                               cap_reply_handle(e), seqs[s]);
                        g_failures++;
                        return;
                    }
                    if (cap_reply_seq(e) != static_cast<uint8_t>(seqs[s]))
                    {
                        printf("not ok - seq 0x%02x came back 0x%02x (handle 0x%08x)\n", seqs[s],
                               cap_reply_seq(e), handle);
                        g_failures++;
                        return;
                    }
                    if (e.type != static_cast<uint8_t>(CapType::CAP_REPLY))
                    {
                        printf("not ok - the sequence overwrote the type (handle 0x%08x)\n",
                               handle);
                        g_failures++;
                        return;
                    }
                    if (e.rights != 0)
                    {
                        printf("not ok - the sequence set a rights bit 0x%02x (handle 0x%08x)\n",
                               e.rights, handle);
                        g_failures++;
                        return;
                    }
                }
            }
        }
        printf("# %u (index, generation, sequence) triples round-trip, %u of them negative\n",
               checked, negative);
        check(negative > 0, "the sample includes handles bit 31 makes negative");
    }

    // A thread index above 255 must not cost a bit of the generation, which is the cross-task
    // ABA counter.
    void case_index_above_the_old_ceiling_keeps_the_full_generation()
    {
        CapEntry e = {};
        uint32_t const handle = (0xFFFFu << 16) | 300u;
        mint(&e, handle, 0xA5u);
        check(cap_reply_handle(e) == handle,
              "thread index 300 with a fully aged generation survives the entry");
        check((cap_reply_handle(e) >> 16) == 0xFFFFu,
              "and all 16 generation bits come back, not 9");
        check(cap_reply_seq(e) == 0xA5u, "with its call sequence beside it");
    }

    // The sequence must not be able to forge a right. It is split across the bytes holding
    // the type and the rights, so a sequence value landing on the CAP_TRANSFER bit would make
    // a one-shot reply cap delegable.
    void case_sequence_can_never_forge_a_right()
    {
        for (uint32_t s = 0; s < 256u; s++)
        {
            CapEntry e = {};
            mint(&e, 1u, static_cast<uint8_t>(s));
            if (e.rights != 0)
            {
                printf("not ok - sequence 0x%02x leaked rights 0x%02x\n", s, e.rights);
                g_failures++;
                return;
            }
            if (e.type != static_cast<uint8_t>(CapType::CAP_REPLY))
            {
                printf("not ok - sequence 0x%02x corrupted the type\n", s);
                g_failures++;
                return;
            }
        }
        // And the reverse: seating every right must leave the sequence alone.
        uint8_t const all = CapRights::CAP_WAIT | CapRights::CAP_SIGNAL | CapRights::CAP_TRANSFER;
        for (uint8_t r = 0; r <= all; r++)
        {
            CapEntry e = {};
            mint(&e, 1u, 0xFFu);
            e.rights = r;
            if (cap_reply_seq(e) != 0xFFu)
            {
                printf("not ok - rights 0x%02x corrupted the sequence\n", r);
                g_failures++;
                return;
            }
        }
        printf("# all 256 sequence values leave the type and the rights untouched\n");
    }

    // The header asserts the field widths; this pins the FOOTPRINT, which is what a re-cut of
    // the bitfields would silently spend.
    void case_entry_footprint()
    {
        printf("# sizeof(CapEntry)=%u alignof(CapEntry)=%u\n",
               static_cast<unsigned>(sizeof(CapEntry)), static_cast<unsigned>(alignof(CapEntry)));
        check(sizeof(CapEntry) == 8, "CapEntry is still 8 bytes with the sequence relocated");
        check(alignof(CapEntry) == 8, "and still 8-aligned");
    }
}

int main()
{
    case_entry_footprint();
    case_round_trip_over_the_whole_word();
    case_index_above_the_old_ceiling_keeps_the_full_generation();
    case_sequence_can_never_forge_a_right();

    if (g_failures != 0)
    {
        printf("FAIL: %d reply-packing check(s) failed\n", g_failures);
        return 1;
    }
    printf("ok - CAP_REPLY carries a full 32-bit thread handle plus its call sequence\n");
    return 0;
}
