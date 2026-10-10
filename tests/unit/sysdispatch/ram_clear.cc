// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kos_ram_alloc over the REAL kernel/syscall/syscall.cc at the region posture: the block the
// arena hands out still holds what was there, and the call clears every byte of the extent a
// region grants over it, not only the bytes asked for.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>

#include <string.h>

#include "kseam_test.h"
#include "region_seam.h"

#if KICKOS_HAVE_ASPACE or not KICKOS_HAVE_MPU
#error "this gate's posture is a region backend"
#endif

extern "C" uint64_t syscall_dispatch(uintptr_t nr, uintptr_t a0, uintptr_t a1, uintptr_t a2,
                                     uintptr_t a3);

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            constexpr unsigned char DIRTY = 0xEE;

            class SysRamClear : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    // Never rewound: ramown.cc keeps every block's owner for the image's life.
                    memset(g_arena + g_arena_used, DIRTY, sizeof(g_arena) - g_arena_used);
                    Thread* const c = seat_pool(1, 10);
                    join_task(c, task(0));
                    cap_seat_authority(c, AUTH_MEMORY);
                    kernel().current(kickos_kernel_core()) = c;
                }
            };

            TEST_F(SysRamClear, a_block_is_cleared_over_the_extent_its_region_grants)
            {
                constexpr size_t ASKED = 100;
                static_assert(ASKED < REGION_MIN, "the extent passes the bytes asked for");
                size_t const at = g_arena_used;
                uintptr_t const block = syscall_dispatch(KOS_SYS_RAM_ALLOC, ASKED, 0, 0, 0);
                ASSERT_EQ(block, reinterpret_cast<uintptr_t>(g_arena + at));
                ASSERT_EQ(g_arena_used, at + REGION_MIN);
                size_t dirty = 0;
                for (size_t i = at; i < at + REGION_MIN; i++)
                {
                    if (g_arena[i] != 0)
                    {
                        dirty++;
                    }
                }
                EXPECT_EQ(dirty, 0u) << "the extent past the bytes asked for kept old bytes";
                EXPECT_EQ(g_arena[at + REGION_MIN], DIRTY) << "the clear ran past the block";
            }

            TEST_F(SysRamClear, a_caller_without_memory_authority_gets_no_block)
            {
                size_t const at = g_arena_used;
                cap_seat_authority(kernel().current(kickos_kernel_core()), 0);
                EXPECT_EQ(syscall_dispatch(KOS_SYS_RAM_ALLOC, REGION_MIN, 0, 0, 0), 0u);
                EXPECT_EQ(g_arena_used, at);
            }
        }
    }
}
