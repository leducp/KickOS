// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KOS_SYS_REPLY_RECV over the REAL kernel/syscall/syscall.cc: a null opts is the reply alone, its
// length the packed reply half, and every other opts reaches the fused reply and receive.

#include <kickos/instance.h>
#include <kickos/kernel.h>
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
            class SysReplyAlone : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    Thread* const caller = seat_pool(1, 10);
                    join_task(caller, task(0));
                    kernel().current(kickos_kernel_core()) = caller;
                    g_replies = 0;
                    g_reply_recvs = 0;
                }

                int reply_recv(uint32_t cap, uintptr_t buf, uintptr_t lens, uintptr_t opts)
                {
                    return static_cast<int>(
                        syscall_dispatch(KOS_SYS_REPLY_RECV, cap, buf, lens, opts));
                }
            };

            TEST_F(SysReplyAlone, a_null_opts_replies_and_receives_nothing)
            {
                unsigned char buf[8] = {};
                uintptr_t const b = reinterpret_cast<uintptr_t>(buf);
                EXPECT_EQ(reply_recv(5u, b, kos_call_lens_pack(3u, 0u), 0u), -KOS_ESRCH)
                    << "the reply's own answer, never -KOS_EINVAL";
                ASSERT_EQ(g_replies, 1u);
                EXPECT_EQ(g_reply_cap, 5u);
                EXPECT_EQ(g_reply_buf, b);
                EXPECT_EQ(g_reply_len, 3u);
                EXPECT_EQ(g_reply_recvs, 0u);
            }

            TEST_F(SysReplyAlone, options_reach_the_fused_reply_and_receive)
            {
                unsigned char buf[8] = {};
                struct kos_reply_recv_opts opts;
                kos_reply_recv_opts_init(&opts, KOS_CAP_NONE, 0u, KOS_TIMEOUT_NONE);
                EXPECT_EQ(reply_recv(5u, reinterpret_cast<uintptr_t>(buf),
                                     kos_call_lens_pack(3u, sizeof(buf)),
                                     reinterpret_cast<uintptr_t>(&opts)),
                          STUB_REPLY_RECV_RC);
                EXPECT_EQ(g_reply_recvs, 1u);
                EXPECT_EQ(g_replies, 0u);
            }
        }
    }
}
