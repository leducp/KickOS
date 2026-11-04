// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KOS_SYS_CLOCK_SET_REALTIME and KOS_SYS_CLOCK_REALTIME over the REAL kernel/syscall/syscall.cc
// and kernel/time/realtime.cc at the region posture: AUTH_REALTIME alone admits a set, a refusal
// leaves the wall clock as it was, and the wall clock moves with the monotonic clock from the set.

#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

#include "kseam_test.h"
#include "stub_seam.h"

extern "C" uint64_t syscall_dispatch(uintptr_t nr, uintptr_t a0, uintptr_t a1, uintptr_t a2,
                                     uintptr_t a3);

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            constexpr uint64_t UNIX_NS = 1700000000123456789ull;

            class SysRealtime : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    g_clock_now = 1000u;
                    caller_ = seat_pool(1, 10);
                    kernel().current(kickos_kernel_core()) = caller_;
                }

                void TearDown() override
                {
                    g_clock_now = 0;
                    KSeam::TearDown();
                }

                int32_t set(uint32_t authority, uint64_t unix_ns)
                {
                    cap_seat_authority(caller_, authority);
                    return static_cast<int32_t>(syscall_dispatch(KOS_SYS_CLOCK_SET_REALTIME,
                                                                 kos_u64_lo(unix_ns),
                                                                 kos_u64_hi(unix_ns), 0, 0));
                }

                uint64_t wall()
                {
                    return syscall_dispatch(KOS_SYS_CLOCK_REALTIME, 0, 0, 0, 0);
                }

                Thread* caller_ = nullptr;
            };

            TEST_F(SysRealtime, every_other_authority_is_refused_and_moves_nothing)
            {
                uint64_t const before = wall();
                EXPECT_EQ(set(0u, UNIX_NS), -KOS_EPERM);
                EXPECT_EQ(set(CAP_AUTH_ALL & ~AUTH_REALTIME, UNIX_NS), -KOS_EPERM);
                EXPECT_EQ(wall(), before);
            }

            TEST_F(SysRealtime, the_bit_alone_sets_the_wall_clock)
            {
                EXPECT_EQ(set(AUTH_REALTIME, UNIX_NS), 0);
                EXPECT_EQ(wall(), UNIX_NS);
            }

            TEST_F(SysRealtime, the_wall_clock_runs_on_the_monotonic_clock)
            {
                ASSERT_EQ(set(AUTH_REALTIME, UNIX_NS), 0);
                g_clock_now = 1000u + 5000u;
                EXPECT_EQ(wall(), UNIX_NS + 5000u);
                EXPECT_EQ(syscall_dispatch(KOS_SYS_CLOCK_NOW, 0, 0, 0, 0), 6000u)
                    << "the monotonic clock never moves with a set";
            }

            TEST_F(SysRealtime, the_last_set_wins)
            {
                ASSERT_EQ(set(AUTH_REALTIME, UNIX_NS), 0);
                ASSERT_EQ(set(AUTH_REALTIME, 5u), 0);
                EXPECT_EQ(wall(), 5u);
            }
        }
    }
}
