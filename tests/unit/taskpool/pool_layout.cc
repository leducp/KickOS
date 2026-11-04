// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Every pooled type's default image is all zero, so the pools TU stores the same bytes whatever
// the order of a type's members, and the layout fingerprint (<kickos/pool_storage.h>) needs only
// each type's size and alignment.

#include "../common/seam_pools.h"

#include <kickos/pool_storage.h>

#include <gtest/gtest.h>

#include <new>
#include <stddef.h>
#include <string.h>

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            // The first byte of `T`'s default image that is not zero, or -1.
            template <class T>
            int non_zero_byte()
            {
                alignas(T) static unsigned char image[sizeof(T)];
                memset(image, 0, sizeof(image));
                new (image) T();
                for (size_t i = 0; i < sizeof(T); i++)
                {
                    if (image[i] != 0)
                    {
                        return static_cast<int>(i);
                    }
                }
                return -1;
            }
        }

        TEST(PoolLayout, every_pooled_type_s_default_image_is_zero)
        {
            EXPECT_EQ(non_zero_byte<Thread>(), -1) << "Thread";
            EXPECT_EQ(non_zero_byte<Task>(), -1) << "Task";
            EXPECT_EQ(non_zero_byte<Domain>(), -1) << "Domain";
            EXPECT_EQ(non_zero_byte<Endpoint>(), -1) << "Endpoint";
            EXPECT_EQ(non_zero_byte<Notification>(), -1) << "Notification";
            EXPECT_EQ(non_zero_byte<IrqBinding>(), -1) << "IrqBinding";
            EXPECT_EQ(non_zero_byte<WordRecord>(), -1) << "WordRecord";
            EXPECT_EQ(non_zero_byte<CapEntry>(), -1) << "CapEntry";
            EXPECT_EQ(non_zero_byte<ListNode>(), -1) << "ListNode";
            EXPECT_EQ(non_zero_byte<ThreadLinks>(), -1) << "ThreadLinks";
            EXPECT_EQ(non_zero_byte<TaskLinks>(), -1) << "TaskLinks";
#if KICKOS_HAVE_ASPACE
            EXPECT_EQ(non_zero_byte<FrameRun>(), -1) << "FrameRun";
            EXPECT_EQ(non_zero_byte<aspace::ResidencyRow>(), -1) << "ResidencyRow";
#if KICKOS_PRESYNC
            EXPECT_EQ(non_zero_byte<PresyncRecord>(), -1) << "PresyncRecord";
#endif
#endif
#if not KICKOS_HAVE_ASPACE
            EXPECT_EQ(non_zero_byte<RamBlock>(), -1) << "RamBlock";
#endif
        }
    }
}
