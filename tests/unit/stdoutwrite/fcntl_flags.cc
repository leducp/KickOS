// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// fcntl on the console's fds: F_SETFL and F_GETFL answer -1 with errno where the kernel refuses
// the task's O_NONBLOCK, never success over a flag that did not move.

#include <gtest/gtest.h>

#include <kickos/sys.h>

#include <errno.h>
#include <fcntl.h>

namespace
{
    int g_answer = 0;
    int g_op = -1;
}

extern "C" int kos_task_nonblock(int op)
{
    g_op = op;
    return g_answer;
}

TEST(FcntlFlags, a_set_the_kernel_refuses_fails)
{
    g_answer = -KOS_EINVAL;
    errno = 0;
    EXPECT_EQ(kickos_test_fcntl(1, F_SETFL, O_NONBLOCK), -1);
    EXPECT_EQ(errno, EINVAL);
    EXPECT_EQ(g_op, KOS_NONBLOCK_SET);
    g_answer = -KOS_ENOSYS;
    errno = 0;
    EXPECT_EQ(kickos_test_fcntl(1, F_SETFL, 0), -1);
    EXPECT_EQ(errno, ENOSYS);
    EXPECT_EQ(g_op, KOS_NONBLOCK_CLEAR);
}

TEST(FcntlFlags, a_read_the_kernel_refuses_fails)
{
    g_answer = -KOS_EINVAL;
    errno = 0;
    EXPECT_EQ(kickos_test_fcntl(2, F_GETFL), -1);
    EXPECT_EQ(errno, EINVAL);
}

TEST(FcntlFlags, an_accepted_set_reads_back)
{
    g_answer = 1;
    EXPECT_EQ(kickos_test_fcntl(1, F_SETFL, O_NONBLOCK), 0);
    EXPECT_EQ(kickos_test_fcntl(1, F_GETFL), O_WRONLY | O_NONBLOCK);
    g_answer = 0;
    EXPECT_EQ(kickos_test_fcntl(0, F_GETFL), O_RDONLY);
}
