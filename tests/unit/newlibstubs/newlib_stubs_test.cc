// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The libc fstat the stubs answer: newlib reads st_mode to decide line buffering and st_blksize
// as a buffer size, so a field left as the caller's stack bytes makes stdout's buffering vary.

#include <gtest/gtest.h>

#include <kickos/sys.h>

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern "C"
{
    int _fstat(int fd, struct stat* st);
    int kos_ut_plain_fstat(int fd, struct stat* st);

    // The syscalls the other stubs reach; no case here calls them.
    int32_t kos_send(kos_cap_t, void const*, size_t)
    {
        abort();
    }
    int32_t kos_kconsole_write(void const*, size_t)
    {
        abort();
    }
    int kos_handle_close(kos_cap_t)
    {
        abort();
    }
    uint64_t kos_clock_now(void)
    {
        abort();
    }
    void kos_yield(void)
    {
        abort();
    }
    void kos_exit(int)
    {
        abort();
    }
}

namespace
{
    void expect_console(int (*call)(int, struct stat*), int fd)
    {
        struct stat st;
        memset(&st, 0xA5, sizeof(st));
        ASSERT_EQ(call(fd, &st), 0);
        EXPECT_TRUE(S_ISCHR(st.st_mode));
        EXPECT_EQ(st.st_mode, static_cast<mode_t>(S_IFCHR));
        EXPECT_EQ(st.st_size, 0);
        EXPECT_EQ(st.st_blksize, 0);
        EXPECT_EQ(st.st_blocks, 0);
    }
}

TEST(NewlibStubs, FstatReportsTheConsoleAsACharacterDeviceWithNothingElseSet)
{
    for (int fd = 0; fd < 3; ++fd)
    {
        expect_console(_fstat, fd);
    }
}

TEST(NewlibStubs, ThePlainNameAnswersAsTheUnderscoredOne)
{
    expect_console(kos_ut_plain_fstat, 1);
}
