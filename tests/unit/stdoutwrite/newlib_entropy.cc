// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The getentropy of user/src/newlib_stubs.cc. On an x86_64 host the stubs take their x86_64 form,
// and a scripted RDSEED stands in for the instruction; elsewhere they have no source.

#include <gtest/gtest.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

extern "C"
{
    int _getentropy(void* buf, size_t len);
}

#if defined(__x86_64__)
namespace
{
    constexpr uint32_t MANY_TRIES = 100000;

    bool g_present = false;
    // The draw of its own on which each word reports ready; the draws before it report none.
    uint32_t g_ready_on = 1;
    uint32_t g_tries = 0;
    uint32_t g_draws = 0;
    uint32_t g_words = 0;

    uint64_t word_for(uint32_t index)
    {
        return 0x0102030405060708ull * (index + 1u);
    }
}

extern "C"
{
    bool kickos_user_rdseed_present(void)
    {
        return g_present;
    }

    bool kickos_user_rdseed(uint64_t* word)
    {
        g_draws = g_draws + 1;
        g_tries = g_tries + 1;
        if (g_tries < g_ready_on)
        {
            *word = 0;
            return false;
        }
        g_tries = 0;
        *word = word_for(g_words);
        g_words = g_words + 1;
        return true;
    }
}

namespace
{
    class NewlibEntropy : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            g_present = true;
            g_ready_on = 1;
            g_tries = 0;
            g_draws = 0;
            g_words = 0;
            errno = 0;
        }
    };

    TEST_F(NewlibEntropy, NoRdseedHasNoSource)
    {
        g_present = false;
        unsigned char buf[16] = {};
        EXPECT_EQ(_getentropy(buf, sizeof(buf)), -1);
        EXPECT_EQ(errno, ENOSYS);
        EXPECT_EQ(g_draws, 0u);
    }

    TEST_F(NewlibEntropy, FillsFromWholeWordsAndATail)
    {
        unsigned char buf[13] = {};
        ASSERT_EQ(_getentropy(buf, sizeof(buf)), 0);
        EXPECT_EQ(g_draws, 2u);
        for (size_t i = 0; i < sizeof(buf); i++)
        {
            uint64_t const word = word_for(static_cast<uint32_t>(i / 8));
            EXPECT_EQ(buf[i], static_cast<unsigned char>(word >> (8u * (i % 8)))) << i;
        }
    }

    TEST_F(NewlibEntropy, AnEmptyRequestDrawsNothing)
    {
        EXPECT_EQ(_getentropy(nullptr, 0), 0);
        EXPECT_EQ(g_draws, 0u);
    }

    TEST_F(NewlibEntropy, AWordNotReadyForManyDrawsStillFills)
    {
        unsigned char buf[16] = {};
        g_ready_on = MANY_TRIES;
        ASSERT_EQ(_getentropy(buf, sizeof(buf)), 0);
        EXPECT_EQ(g_draws, 2u * MANY_TRIES);
        EXPECT_EQ(buf[0], static_cast<unsigned char>(word_for(0)));
        EXPECT_EQ(buf[8], static_cast<unsigned char>(word_for(1)));
    }

    TEST_F(NewlibEntropy, ServesUpTo256Bytes)
    {
        unsigned char buf[257] = {};
        ASSERT_EQ(_getentropy(buf, 256), 0);
        EXPECT_EQ(g_draws, 32u);
        g_draws = 0;
        EXPECT_EQ(_getentropy(buf, 257), -1);
        EXPECT_EQ(errno, EINVAL);
        EXPECT_EQ(g_draws, 0u);
    }
}
#else
namespace
{
    TEST(NewlibEntropy, HasNoSource)
    {
        unsigned char buf[16] = {};
        EXPECT_EQ(_getentropy(buf, sizeof(buf)), -1);
        EXPECT_EQ(errno, ENOSYS);
    }
}
#endif
