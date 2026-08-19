// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// lib/libc/string.cc's word-wise core, over every alignment pair, both sides of the
// word path's entry threshold, and every overlap shape memmove has to survive.
// The reference copies are written out by hand: routing them through the host libc
// would compare an implementation against itself once the shim's macros are in scope.

#include <gtest/gtest.h>

#include <stddef.h>
#include <stdint.h>

extern "C"
{
    void* kos_ut_memcpy(void* dst, void const* src, size_t n);
    void* kos_ut_memset(void* dst, int c, size_t n);
    void* kos_ut_memmove(void* dst, void const* src, size_t n);
    int kos_ut_memcmp(void const* a, void const* b, size_t n);
    size_t kos_ut_strlen(char const* s);
    size_t kos_ut_strnlen(char const* s, size_t maxlen);
}

namespace
{
    // The word the implementation uses, so the alignment sweep and the threshold below
    // track it rather than assuming 32 bits.
    constexpr size_t WORD_BYTES = sizeof(unsigned long);
    constexpr size_t WORD_MIN = 2u * WORD_BYTES;

    // Wide enough that a copy at the largest offset still crosses several words, and
    // over-allocated at both ends so an overrun lands in a guard byte rather than a
    // neighbouring test object.
    constexpr size_t GUARD = 2u * WORD_MIN;
    constexpr size_t ARENA = 512u;

    unsigned char fill_byte(size_t i)
    {
        return static_cast<unsigned char>((i * 7u + 13u) & 0xffu);
    }

    struct Arena
    {
        // Over-aligned so an offset added to data() is the ONLY misalignment in play.
        alignas(64) unsigned char raw[ARENA + 2u * GUARD];

        unsigned char* at(size_t off) { return raw + GUARD + off; }

        void seed()
        {
            for (size_t i = 0; i < sizeof(raw); i++)
            {
                raw[i] = fill_byte(i);
            }
        }
        void expect_guards_intact() const
        {
            for (size_t i = 0; i < GUARD; i++)
            {
                ASSERT_EQ(raw[i], fill_byte(i)) << "underrun at " << i;
            }
            for (size_t i = sizeof(raw) - GUARD; i < sizeof(raw); i++)
            {
                ASSERT_EQ(raw[i], fill_byte(i)) << "overrun at " << i;
            }
        }
    };

    // Sizes either side of the word path's entry threshold, plus a run of small n and a
    // few that leave a head and a tail on every alignment.
    std::vector<size_t> sizes()
    {
        std::vector<size_t> v;
        for (size_t n = 0; n <= WORD_MIN + 2u; n++)
        {
            v.push_back(n);
        }
        for (size_t n : {17u, 31u, 32u, 33u, 63u, 64u, 65u, 127u, 255u})
        {
            v.push_back(n);
        }
        return v;
    }
}

TEST(KStringMemcpy, EveryAlignmentPairAndSize)
{
    // 2*WORD_BYTES offsets cover every residue AND the case where the two operands share
    // a residue at a different absolute alignment.
    for (size_t doff = 0; doff < 2u * WORD_BYTES; doff++)
    {
        for (size_t soff = 0; soff < 2u * WORD_BYTES; soff++)
        {
            for (size_t n : sizes())
            {
                static Arena dst;
                static Arena src;
                dst.seed();
                src.seed();
                for (size_t i = 0; i < n; i++)
                {
                    src.at(soff)[i] = static_cast<unsigned char>(i ^ 0xa5u);
                }

                void* rc = kos_ut_memcpy(dst.at(doff), src.at(soff), n);
                ASSERT_EQ(rc, dst.at(doff)) << "memcpy must return dst";

                for (size_t i = 0; i < n; i++)
                {
                    ASSERT_EQ(dst.at(doff)[i], static_cast<unsigned char>(i ^ 0xa5u))
                        << "d=" << doff << " s=" << soff << " n=" << n << " at " << i;
                }
                // One byte either side of the destination range must be untouched.
                ASSERT_EQ(dst.at(doff)[-1], fill_byte(GUARD + doff - 1u));
                ASSERT_EQ(dst.at(doff)[n], fill_byte(GUARD + doff + n));
                dst.expect_guards_intact();
            }
        }
    }
}

TEST(KStringMemset, EveryAlignmentAndSize)
{
    for (size_t doff = 0; doff < 2u * WORD_BYTES; doff++)
    {
        for (size_t n : sizes())
        {
            static Arena dst;
            dst.seed();

            void* rc = kos_ut_memset(dst.at(doff), 0x5a, n);
            ASSERT_EQ(rc, dst.at(doff)) << "memset must return dst";

            for (size_t i = 0; i < n; i++)
            {
                ASSERT_EQ(dst.at(doff)[i], 0x5au) << "d=" << doff << " n=" << n << " at " << i;
            }
            ASSERT_EQ(dst.at(doff)[-1], fill_byte(GUARD + doff - 1u));
            ASSERT_EQ(dst.at(doff)[n], fill_byte(GUARD + doff + n));
            dst.expect_guards_intact();
        }
    }
}

// A value with a high bit set catches a fill word built by a SIGNED shift, and one above
// 0xff catches a missing narrowing to the low byte.
TEST(KStringMemset, TruncatesToLowByteAndFillsHighBits)
{
    static Arena dst;
    dst.seed();
    kos_ut_memset(dst.at(1), 0x1ff, 64);
    for (size_t i = 0; i < 64; i++)
    {
        ASSERT_EQ(dst.at(1)[i], 0xffu) << "at " << i;
    }
    dst.seed();
    kos_ut_memset(dst.at(3), 0x80, 40);
    for (size_t i = 0; i < 40; i++)
    {
        ASSERT_EQ(dst.at(3)[i], 0x80u) << "at " << i;
    }
}

TEST(KStringMemmove, OverlapBothDirectionsAtEveryDistance)
{
    // delta 0 is the aliased case; the rest sweep the source below and above the
    // destination at every distance a word loop could read after it had written.
    for (size_t off = 0; off < 2u * WORD_BYTES; off++)
    {
        for (int delta = -2 * static_cast<int>(WORD_MIN); delta <= 2 * static_cast<int>(WORD_MIN); delta++)
        {
            for (size_t n : {size_t{0}, size_t{1}, WORD_MIN - 1u, WORD_MIN, WORD_MIN + 1u, size_t{64}, size_t{129}})
            {
                static Arena a;
                a.seed();
                unsigned char* d = a.at(off + WORD_MIN * 4u);
                unsigned char const* s = d + delta;

                // The expected result, taken from the arena BEFORE the move.
                std::vector<unsigned char> want(n);
                for (size_t i = 0; i < n; i++)
                {
                    want[i] = s[i];
                }

                void* rc = kos_ut_memmove(d, s, n);
                ASSERT_EQ(rc, d) << "memmove must return dst";
                for (size_t i = 0; i < n; i++)
                {
                    ASSERT_EQ(d[i], want[i])
                        << "off=" << off << " delta=" << delta << " n=" << n << " at " << i;
                }
                a.expect_guards_intact();
            }
        }
    }
}

// memcpy has no overlap guarantee, so this pins only what the seam in
// kernel/syscall/syscall_mem.cc actually relies on: ascending copy is correct when the
// destination is BELOW the source, overlap or not.
TEST(KStringMemcpy, AscendingIsCorrectWhenDstBelowSrc)
{
    for (size_t off = 0; off < 2u * WORD_BYTES; off++)
    {
        for (size_t gap = 1; gap <= 2u * WORD_MIN; gap++)
        {
            static Arena a;
            a.seed();
            constexpr size_t N = 96;
            unsigned char* d = a.at(off + WORD_MIN * 4u);
            unsigned char const* s = d + gap;

            std::vector<unsigned char> want(N);
            for (size_t i = 0; i < N; i++)
            {
                want[i] = s[i];
            }
            kos_ut_memcpy(d, s, N);
            for (size_t i = 0; i < N; i++)
            {
                ASSERT_EQ(d[i], want[i]) << "off=" << off << " gap=" << gap << " at " << i;
            }
        }
    }
}

TEST(KStringMemcpy, ZeroLengthTouchesNothing)
{
    static Arena dst;
    dst.seed();
    unsigned char before = dst.at(1)[0];
    ASSERT_EQ(kos_ut_memcpy(dst.at(1), dst.at(64), 0), dst.at(1));
    ASSERT_EQ(dst.at(1)[0], before);
    ASSERT_EQ(kos_ut_memset(dst.at(1), 0, 0), dst.at(1));
    ASSERT_EQ(dst.at(1)[0], before);
    ASSERT_EQ(kos_ut_memmove(dst.at(1), dst.at(64), 0), dst.at(1));
    ASSERT_EQ(dst.at(1)[0], before);
    dst.expect_guards_intact();
}

TEST(KStringCompareAndLength, UnchangedPrimitives)
{
    unsigned char a[] = {1, 2, 3, 4};
    unsigned char b[] = {1, 2, 4, 4};
    EXPECT_EQ(kos_ut_memcmp(a, b, 2), 0);
    EXPECT_LT(kos_ut_memcmp(a, b, 4), 0);
    EXPECT_GT(kos_ut_memcmp(b, a, 4), 0);
    EXPECT_EQ(kos_ut_memcmp(a, b, 0), 0);
    EXPECT_EQ(kos_ut_strlen(""), 0u);
    EXPECT_EQ(kos_ut_strlen("abcd"), 4u);
    EXPECT_EQ(kos_ut_strnlen("abcd", 2), 2u);
    EXPECT_EQ(kos_ut_strnlen("ab", 9), 2u);
}
