// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// fcntl on the console's fds: F_SETFL moves this image's O_NONBLOCK word, which stdout_write
// reads, and F_GETFL reads it back.

#include <gtest/gtest.h>

#include <kickos/sys/emit.h>

#include <errno.h>
#include <fcntl.h>

TEST(FcntlFlags, a_set_reads_back_on_every_console_fd)
{
    EXPECT_EQ(kickos_test_fcntl(1, F_GETFL), O_WRONLY);
    EXPECT_EQ(kickos_test_fcntl(1, F_SETFL, O_NONBLOCK), 0);
    EXPECT_EQ(kickos_test_fcntl(1, F_GETFL), O_WRONLY | O_NONBLOCK);
    EXPECT_EQ(kickos_test_fcntl(2, F_GETFL), O_WRONLY | O_NONBLOCK);
    EXPECT_EQ(kickos_test_fcntl(0, F_GETFL), O_RDONLY | O_NONBLOCK);
    EXPECT_EQ(kickos::stdout_nonblock(), 1u);
}

TEST(FcntlFlags, a_clear_reads_back)
{
    EXPECT_EQ(kickos_test_fcntl(1, F_SETFL, 0), 0);
    EXPECT_EQ(kickos_test_fcntl(1, F_GETFL), O_WRONLY);
    EXPECT_EQ(kickos::stdout_nonblock(), 0u);
}

TEST(FcntlFlags, an_fd_beyond_the_console_is_refused)
{
    errno = 0;
    EXPECT_EQ(kickos_test_fcntl(3, F_SETFL, O_NONBLOCK), -1);
    EXPECT_EQ(errno, EBADF);
    EXPECT_EQ(kickos::stdout_nonblock(), 0u);
}
