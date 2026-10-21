// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/reent.h>

#include <gtest/gtest.h>

#include <stdint.h>

#if not KICKOS_LIBC_REENT or not KICKOS_REENT_IN_TCB
#error "this gate's posture is the reent state in the TLS control block"
#endif

extern "C"
{
    KickosReentSeam const kickos_reent_seam = {};
}

namespace
{
    constexpr uintptr_t UNWRITTEN = 0xA5A5A5A5u;

    TEST(ReentTcb, the_control_block_holds_the_state_then_the_id)
    {
        alignas(sizeof(void*)) uintptr_t block[KICKOS_ARCH_TLS_TCB / sizeof(void*) + 1];
        for (uintptr_t& w : block)
        {
            w = UNWRITTEN;
        }
        unsigned char state[8];
        uint32_t const id = 0x12345u;
        kickos::reent_seat_tcb(block, state, id);
        EXPECT_EQ(block[0], reinterpret_cast<uintptr_t>(state));
        EXPECT_EQ(block[1], id);
        for (size_t i = 2; i < sizeof(block) / sizeof(block[0]); i++)
        {
            EXPECT_EQ(block[i], UNWRITTEN) << "word " << i << " is past the seat";
        }
    }
}
