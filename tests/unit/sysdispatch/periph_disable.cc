// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KOS_SYS_PERIPH_DISABLE over the REAL kernel/syscall/syscall.cc at the region posture: the
// caller's device window at the exact base is the whole gate, and the seam's answer is the
// caller's, -KOS_ENOSYS included, which is what every chip without a backend answers. The
// recorder stands in for the seam: the generated default cannot link beside it.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

#include "kseam_test.h"

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
            constexpr uintptr_t WINDOW = 0x4002C000u;
            constexpr size_t SIZE = 0x40u;

            class SysPeriphDisable : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    caller_ = seat_pool(1, 10);
                    join_task(caller_, task(0));
                    kernel().current(kickos_kernel_core()) = caller_;
                }

                int disable(uintptr_t base)
                {
                    return static_cast<int>(
                        syscall_dispatch(KOS_SYS_PERIPH_DISABLE, base, 0, 0, 0));
                }

                Thread* caller_ = nullptr;
            };

            TEST_F(SysPeriphDisable, a_holder_reaches_the_seam_once_and_gets_its_answer)
            {
                ASSERT_TRUE(caller_->mpu.add(WINDOW, SIZE, ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_DEV));
                EXPECT_EQ(disable(WINDOW), 0);
                ASSERT_EQ(g_periph_disables, 1u);
                EXPECT_EQ(g_periph_disabled[0], WINDOW);

                g_periph_disable_rc = -KOS_ENOSYS;
                EXPECT_EQ(disable(WINDOW), -KOS_ENOSYS) << "a chip with no backend says so";
                EXPECT_EQ(g_periph_disables, 2u);
            }

            TEST_F(SysPeriphDisable, possession_is_the_whole_gate)
            {
                EXPECT_EQ(disable(WINDOW), -KOS_EPERM) << "no window at all";
                ASSERT_TRUE(caller_->mpu.add(WINDOW, SIZE, ARCH_MPU_R | ARCH_MPU_W));
                EXPECT_EQ(disable(WINDOW), -KOS_EPERM) << "a memory region at the base";
                ASSERT_TRUE(
                    caller_->mpu.add(WINDOW + SIZE, SIZE, ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_DEV));
                EXPECT_EQ(disable(WINDOW + SIZE + 4u), -KOS_EPERM)
                    << "inside a window, not its base";
                EXPECT_EQ(g_periph_disables, 0u) << "a refusal reaches no seam";
            }
        }
    }
}
