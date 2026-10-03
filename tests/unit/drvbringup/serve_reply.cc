// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The driver witness's serve loop (tests/drivers/serve.h) against a caller whose timed call
// expires after its request is taken: the reply answers -KOS_ESRCH, which ends that one call and
// not the driver, so the instance still answers SERVED calls, counted 1 to SERVED.

#include "../../drivers/serve.h"

#include <kickos/sys/errno.h>

#include <gtest/gtest.h>

#include <stdint.h>

namespace
{
    constexpr kos_cap_t REPLY_CAP = 40u;

    // How many replies answer -KOS_ESRCH before the callers wait for theirs.
    uint32_t g_gone = 0u;
    uint32_t g_answered[16] = {};
    uint32_t g_answer_count = 0u;
    uint32_t g_receives = 0u;
}

extern "C"
{
    int kos_reply_recv(kos_cap_t, void*, uintptr_t, struct kos_reply_recv_opts* opts)
    {
        g_receives++;
        if (g_receives > 32u)
        {
            return -KOS_EBADF;
        }
        opts->info.reply_cap = REPLY_CAP;
        return 0;
    }

    int kos_reply(kos_cap_t reply_cap, void const* buf, size_t len)
    {
        EXPECT_EQ(reply_cap, REPLY_CAP);
        EXPECT_EQ(len, sizeof(uint32_t));
        if (g_gone != 0u)
        {
            g_gone--;
            return -KOS_ESRCH;
        }
        if (g_answer_count < 16u)
        {
            g_answered[g_answer_count] = *static_cast<uint32_t const*>(buf);
        }
        g_answer_count++;
        return 0;
    }
}

namespace
{
    TEST(DriverWitnessServe, a_caller_gone_before_its_reply_does_not_end_the_instance)
    {
        g_gone = 2u;
        EXPECT_EQ(testdrivers::serve(7u), 0);
        ASSERT_EQ(g_answer_count, testdrivers::SERVED);
        for (uint32_t i = 0; i < testdrivers::SERVED; i++)
        {
            EXPECT_EQ(g_answered[i], i + 1u);
        }
        EXPECT_EQ(g_receives, testdrivers::SERVED + 2u);
    }
}
