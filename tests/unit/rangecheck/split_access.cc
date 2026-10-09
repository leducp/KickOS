// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A user range across a granule boundary, over the REAL kernel/syscall/syscall_mem.cc: the two
// virtually adjacent pages are backed by frames that are not adjacent, the frame between them
// mapped nowhere, so a copy that does not split at the granule lands in that frame.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kickos/arch/arch.h>
#include <kickos/aspace.h>

#if not KICKOS_HAVE_ASPACE
#error "this gate's posture is a translating backend"
#endif

#include <gtest/gtest.h>

#include "aspace_seam.h"

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            constexpr size_t PAGE = 0x1000u;
            constexpr size_t HALF = 8;
            constexpr uintptr_t VA = 0x20000000u;
            constexpr uintptr_t CROSS = VA + PAGE - HALF;

            // Three frames a space: the first and the last backing VA and VA + PAGE, the middle
            // one backing nothing.
            alignas(PAGE) unsigned char g_frames[3][3 * PAGE];
            unsigned char g_spaces[3];

            struct arch_aspace* space(size_t i)
            {
                return reinterpret_cast<struct arch_aspace*>(&g_spaces[i]);
            }

            unsigned char* lo(size_t i)
            {
                return &g_frames[i][PAGE - HALF];
            }

            unsigned char* hi(size_t i)
            {
                return &g_frames[i][2 * PAGE];
            }

            bool spill_untouched(size_t i)
            {
                for (size_t b = 0; b < PAGE; b++)
                {
                    if (g_frames[i][PAGE + b] != 0)
                    {
                        return false;
                    }
                }
                return true;
            }

            class SplitAccess : public ::testing::Test
            {
              protected:
                void SetUp() override
                {
                    ASSERT_EQ(arch_aspace_granule(), PAGE);
                    memset(g_frames, 0, sizeof(g_frames));
                    unback_all();
                    for (size_t i = 0; i < 2; i++)
                    {
                        ASSERT_TRUE(back_page(space(i), VA, &g_frames[i][0], false));
                        ASSERT_TRUE(back_page(space(i), VA + PAGE, &g_frames[i][2 * PAGE], false));
                    }
                    // The third space backs its first page alone.
                    ASSERT_TRUE(back_page(space(2), VA, &g_frames[2][0], false));
                    for (size_t i = 0; i < sizeof(pat_); i++)
                    {
                        pat_[i] = static_cast<unsigned char>(0x40u + i);
                    }
                }

                void TearDown() override
                {
                    EXPECT_EQ(holds().live, 0u) << "an acquire was never released";
                    EXPECT_EQ(holds().unpaired, 0u) << "a release met no acquire";
                    EXPECT_EQ(holds_refused_past_min(), 0u)
                        << "a kernel path held more pages at once than ARCH_ASPACE_ACQUIRE_MIN";
                    unback_all();
                }

                unsigned char pat_[2 * HALF] = {};
            };

            TEST_F(SplitAccess, a_write_across_the_boundary_lands_in_each_page_s_frame)
            {
                ASSERT_TRUE(kaccess_to_user(space(0), CROSS, pat_, sizeof(pat_)));
                EXPECT_EQ(memcmp(lo(0), pat_, HALF), 0);
                EXPECT_EQ(memcmp(hi(0), pat_ + HALF, HALF), 0);
                EXPECT_TRUE(spill_untouched(0)) << "the copy ran on past its first frame";
            }

            TEST_F(SplitAccess, a_read_across_the_boundary_reads_each_page_s_frame)
            {
                memcpy(lo(0), pat_, HALF);
                memcpy(hi(0), pat_ + HALF, HALF);
                memset(&g_frames[0][PAGE], 0xEE, PAGE);
                unsigned char back[2 * HALF] = {};
                ASSERT_TRUE(kaccess_from_user(back, space(0), CROSS, sizeof(back)));
                EXPECT_EQ(memcmp(back, pat_, sizeof(back)), 0)
                    << "the copy read on past its first frame";
            }

            TEST_F(SplitAccess, a_copy_between_two_spaces_at_one_address_moves_each_page)
            {
                memcpy(lo(1), pat_, HALF);
                memcpy(hi(1), pat_ + HALF, HALF);
                ASSERT_TRUE(ep_copy(space(0), CROSS, space(1), CROSS, sizeof(pat_)));
                EXPECT_EQ(memcmp(lo(0), pat_, HALF), 0);
                EXPECT_EQ(memcmp(hi(0), pat_ + HALF, HALF), 0);
                EXPECT_TRUE(spill_untouched(0));
                EXPECT_TRUE(spill_untouched(1));
                void* const word = &g_spaces[0];
                ASSERT_TRUE(kaccess_word_to_user(space(0), VA, &word));
                EXPECT_EQ(holds().peak, static_cast<size_t>(ARCH_ASPACE_ACQUIRE_MIN))
                    << "the copy between two spaces holds one page of each end at once";
            }

            TEST_F(SplitAccess, a_copy_refused_at_its_second_page_gives_back_every_hold)
            {
                EXPECT_FALSE(ep_copy(space(0), CROSS, space(2), CROSS, sizeof(pat_)));
                EXPECT_FALSE(ep_copy(space(2), CROSS, space(0), CROSS, sizeof(pat_)));
                EXPECT_FALSE(kaccess_to_user(space(2), CROSS, pat_, sizeof(pat_)));
                EXPECT_TRUE(spill_untouched(2));
            }
        }
    }
}
