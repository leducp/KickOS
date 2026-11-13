// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The fds of user/src/newlib_stubs.cc, through newlib_stubs_shim.cc's renames: fds 0 to 2 are the
// console, a terminal that reads nothing, until each is closed; every other fd is not open.

#include <gtest/gtest.h>

#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include <kickos/sys/emit.h>

extern "C"
{
    int _write(int fd, char const* buf, int len);
    int _read(int fd, char* buf, int len);
    int _close(int fd);
    int _isatty(int fd);
    int _lseek(int fd, int offset, int whence);
    int _fstat(int fd, struct stat* st);
    int kos_ut_plain_close(int fd);
    int kos_ut_plain_isatty(int fd);
    int kos_ut_plain_lseek(int fd, int offset, int whence);
}

namespace
{
    constexpr int NOT_OPEN[] = {-1, 3, 7, 1000};

    class NewlibFd : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            kickos::console_fds() = 0x7u;
            errno = 0;
        }

        void TearDown() override
        {
            kickos::console_fds() = 0x7u;
        }
    };

    void expect_not_open(int fd)
    {
        char byte = 0;
        struct stat st;
        errno = 0;
        EXPECT_EQ(_write(fd, "x", 1), -1);
        EXPECT_EQ(errno, EBADF);
        errno = 0;
        EXPECT_EQ(_read(fd, &byte, 1), -1);
        EXPECT_EQ(errno, EBADF);
        errno = 0;
        EXPECT_EQ(_isatty(fd), 0);
        EXPECT_EQ(errno, EBADF);
        errno = 0;
        EXPECT_EQ(_lseek(fd, 0, SEEK_SET), -1);
        EXPECT_EQ(errno, EBADF);
        errno = 0;
        EXPECT_EQ(_fstat(fd, &st), -1);
        EXPECT_EQ(errno, EBADF);
        errno = 0;
        EXPECT_EQ(_close(fd), -1);
        EXPECT_EQ(errno, EBADF);
    }

    TEST_F(NewlibFd, EveryConsoleFdIsATerminal)
    {
        for (int fd = 0; fd < 3; ++fd)
        {
            EXPECT_EQ(_isatty(fd), 1) << "fd " << fd;
        }
        EXPECT_EQ(kos_ut_plain_isatty(1), 1);
    }

    TEST_F(NewlibFd, ASeekOnTheConsoleIsRefusedAsOnAPipe)
    {
        for (int fd = 0; fd < 3; ++fd)
        {
            for (int whence : {SEEK_SET, SEEK_CUR, SEEK_END})
            {
                errno = 0;
                EXPECT_EQ(_lseek(fd, 100, whence), -1) << "fd " << fd;
                EXPECT_EQ(errno, ESPIPE) << "fd " << fd;
            }
        }
        errno = 0;
        EXPECT_EQ(kos_ut_plain_lseek(1, 0, SEEK_CUR), -1);
        EXPECT_EQ(errno, ESPIPE);
    }

    TEST_F(NewlibFd, StdinReadsEndOfFile)
    {
        char byte = 'x';
        EXPECT_EQ(_read(0, &byte, 1), 0);
        EXPECT_EQ(byte, 'x');
    }

    TEST_F(NewlibFd, EachFdMovesInOneDirectionOnly)
    {
        char byte = 0;
        EXPECT_EQ(_write(0, "x", 1), -1);
        EXPECT_EQ(errno, EBADF);
        for (int fd = 1; fd < 3; ++fd)
        {
            errno = 0;
            EXPECT_EQ(_read(fd, &byte, 1), -1) << "fd " << fd;
            EXPECT_EQ(errno, EBADF) << "fd " << fd;
        }
    }

    TEST_F(NewlibFd, AnFdBeyondTheConsoleIsNotOpen)
    {
        for (int fd : NOT_OPEN)
        {
            SCOPED_TRACE(fd);
            expect_not_open(fd);
        }
    }

    TEST_F(NewlibFd, AClosedConsoleFdIsNotOpenAndTheOthersStay)
    {
        EXPECT_EQ(_close(1), 0);
        expect_not_open(1);
        EXPECT_EQ(_isatty(0), 1);
        EXPECT_EQ(_isatty(2), 1);
        EXPECT_EQ(kos_ut_plain_close(2), 0);
        errno = 0;
        EXPECT_EQ(kos_ut_plain_close(2), -1);
        EXPECT_EQ(errno, EBADF);
        EXPECT_EQ(_isatty(0), 1);
        EXPECT_EQ(_close(0), 0);
        expect_not_open(0);
        EXPECT_EQ(kickos::console_fds(), 0u);
    }
}
