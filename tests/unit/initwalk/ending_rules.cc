// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The init's ending with user/apps/ending's wrap of kos_shutdown linked, as every image of
// user/apps links it on a chip with a bootloader entry.

#include "harness.h"

#include <kickos/sys/errno.h>

#include <gtest/gtest.h>

namespace
{
    using harness::Outcome;

    void end_main(int code)
    {
        fake::reset(harness::config(systems::find("default_qemu_arm64")));
        fake::script({step::end("main", code)});
    }

    TEST(Ending, the_end_hands_the_board_to_the_bootloader)
    {
        end_main(3);
        Outcome const outcome = harness::run(systems::find("default_qemu_arm64"));
        ASSERT_EQ(outcome.kind, Outcome::Kind::SHUTDOWN) << outcome.message;
        EXPECT_EQ(outcome.status, 3);
        std::vector<fake::Call> const asks = fake::calls_of("kos_shutdown");
        ASSERT_EQ(asks.size(), 1u);
        EXPECT_EQ(asks[0].args[0], 3u);
        EXPECT_EQ(asks[0].args[1], static_cast<uint64_t>(KOS_SHUTDOWN_BOOTLOADER));
    }

    TEST(Ending, a_refused_bootloader_leaves_the_halt)
    {
        end_main(3);
        fake::refuse("kos_shutdown", -KOS_EPERM);
        Outcome const outcome = harness::run(systems::find("default_qemu_arm64"));
        ASSERT_EQ(outcome.kind, Outcome::Kind::SHUTDOWN) << outcome.message;
        EXPECT_EQ(outcome.status, 3);
        std::vector<fake::Call> const asks = fake::calls_of("kos_shutdown");
        ASSERT_EQ(asks.size(), 2u);
        EXPECT_EQ(asks[0].args[1], static_cast<uint64_t>(KOS_SHUTDOWN_BOOTLOADER));
        EXPECT_EQ(asks[1].args[0], 3u);
        EXPECT_EQ(asks[1].args[1], static_cast<uint64_t>(KOS_SHUTDOWN_HALT));
    }
}
