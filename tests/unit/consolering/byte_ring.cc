// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sys/byte_ring.h>

#include <gtest/gtest.h>

namespace
{
    unsigned char const SRC[4] = {'a', 'b', 'c', 'd'};

    TEST(ByteRing, capacity_is_one_short_of_the_size)
    {
        unsigned char buf[8];
        struct kos_byte_ring r;
        kos_byte_ring_init(&r, buf, sizeof(buf));
        EXPECT_EQ(kos_byte_ring_used(&r), 0u);
        EXPECT_EQ(kos_byte_ring_space(&r), 7u);

        EXPECT_EQ(kos_byte_ring_push(&r, SRC, 4), 4u);
        EXPECT_EQ(kos_byte_ring_used(&r), 4u);
        EXPECT_EQ(kos_byte_ring_space(&r), 3u);
    }

    TEST(ByteRing, a_full_ring_accepts_short_rather_than_failing_or_dropping)
    {
        unsigned char buf[8];
        struct kos_byte_ring r;
        kos_byte_ring_init(&r, buf, sizeof(buf));
        ASSERT_EQ(kos_byte_ring_push(&r, SRC, 4), 4u);
        EXPECT_EQ(kos_byte_ring_push(&r, SRC, 4), 3u);
        EXPECT_EQ(kos_byte_ring_space(&r), 0u);
        EXPECT_EQ(kos_byte_ring_push(&r, SRC, 1), 0u);
    }

    // The bytes pushed after the first pop straddle the end of the buffer, so a mask bug
    // shows up as wrong ORDER rather than as a bad count.
    TEST(ByteRing, bytes_come_out_in_order_across_the_wrap)
    {
        unsigned char buf[8];
        struct kos_byte_ring r;
        kos_byte_ring_init(&r, buf, sizeof(buf));
        ASSERT_EQ(kos_byte_ring_push(&r, SRC, 4), 4u);
        ASSERT_EQ(kos_byte_ring_push(&r, SRC, 4), 3u);

        unsigned char out[8] = {0};
        EXPECT_EQ(kos_byte_ring_pop(&r, out, 2), 2u);
        EXPECT_EQ(out[0], 'a');
        EXPECT_EQ(out[1], 'b');
        EXPECT_EQ(kos_byte_ring_push(&r, SRC, 2), 2u);
        unsigned char one = 0;
        EXPECT_EQ(kos_byte_ring_pop_one(&r, &one), 1);
        EXPECT_EQ(one, 'c');
        EXPECT_EQ(kos_byte_ring_pop(&r, out, sizeof(out)), 6u);
        unsigned char const want[6] = {'d', 'a', 'b', 'c', 'a', 'b'};
        for (int i = 0; i < 6; i++)
        {
            EXPECT_EQ(out[i], want[i]) << "byte " << i;
        }
        EXPECT_EQ(kos_byte_ring_used(&r), 0u);
        EXPECT_EQ(kos_byte_ring_pop_one(&r, &one), 0);
    }

    TEST(ByteRing, a_non_power_of_two_size_is_refused_as_empty_and_full)
    {
        unsigned char buf[8];
        unsigned char const src = 'a';
        struct kos_byte_ring bad;
        kos_byte_ring_init(&bad, buf, 6);
        EXPECT_EQ(kos_byte_ring_space(&bad), 0u);
        EXPECT_EQ(kos_byte_ring_push(&bad, &src, 1), 0u);
    }
}
