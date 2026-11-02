// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KOS_SYS_KCONSOLE_WRITE's flags over the REAL kernel/syscall/syscall.cc at the region posture:
// the kernel console waits unless the caller passes KOS_KCONSOLE_NOWAIT, and any other flag is
// refused before a byte is read.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

#include "kseam_test.h"
#include "stub_seam.h"

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
            alignas(64) char g_text[64] = "line\n";

            class SysKconsoleFlags : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    caller_ = seat_pool(1, 10);
                    join_task(caller_, task(0));
                    kernel().current(kickos_kernel_core()) = caller_;
                    g_kconsole_writes = 0;
                    g_kconsole_waits = false;
                    ASSERT_TRUE(caller_->mpu.add(reinterpret_cast<uintptr_t>(g_text),
                                                 sizeof(g_text), ARCH_MPU_R));
                }

                int32_t write(uintptr_t flags)
                {
                    return static_cast<int32_t>(
                        syscall_dispatch(KOS_SYS_KCONSOLE_WRITE,
                                         reinterpret_cast<uintptr_t>(g_text), 5, flags, 0));
                }

                Thread* caller_ = nullptr;
            };

            TEST_F(SysKconsoleFlags, no_flag_waits_for_room)
            {
                EXPECT_EQ(write(0), 5);
                EXPECT_EQ(g_kconsole_writes, 1u);
                EXPECT_TRUE(g_kconsole_waits);
            }

            TEST_F(SysKconsoleFlags, the_no_wait_flag_never_waits)
            {
                EXPECT_EQ(write(KOS_KCONSOLE_NOWAIT), 5);
                EXPECT_EQ(g_kconsole_writes, 1u);
                EXPECT_FALSE(g_kconsole_waits);
            }

            TEST_F(SysKconsoleFlags, an_unknown_flag_is_refused_before_the_console)
            {
                EXPECT_EQ(write(KOS_KCONSOLE_NOWAIT << 1), -KOS_EINVAL);
                EXPECT_EQ(g_kconsole_writes, 0u);
            }
        }
    }
}
