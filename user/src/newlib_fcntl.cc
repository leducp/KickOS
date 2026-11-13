// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// newlib's fcntl for the console's fds. NOT compiled for the sim, where host glibc provides it.

#include <kickos/sys/emit.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>

namespace
{
    // fds 0 to 2 share one description, the console, whose O_NONBLOCK is this task's
    // (kickos::stdout_nonblock).
    int fd_fcntl(int fd, int cmd, va_list ap)
    {
        if (not kickos::console_fd_open(fd))
        {
            errno = EBADF;
            return -1;
        }
        switch (cmd)
        {
            case F_GETFL:
            {
                int access = O_WRONLY;
                if (fd == 0)
                {
                    access = O_RDONLY;
                }
                if (kickos::stdout_nonblock() != 0u)
                {
                    return access | O_NONBLOCK;
                }
                return access;
            }
            case F_SETFL:
            {
                uint32_t flag = 0u;
                if ((va_arg(ap, int) & O_NONBLOCK) != 0)
                {
                    flag = 1u;
                }
                kickos::stdout_nonblock() = flag;
                return 0;
            }
            case F_GETFD:
            {
                return 0;
            }
            case F_SETFD:
            {
                (void)va_arg(ap, int);
                return 0;
            }
            default:
            {
                errno = EINVAL;
                return -1;
            }
        }
    }
}

extern "C"
{

// newlib's fcntl reaches this through _fcntl_r.
int _fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    int const rc = fd_fcntl(fd, cmd, ap);
    va_end(ap);
    return rc;
}

// Defined here, not left to libc: newlib-nano's answers ENOSYS without calling _fcntl, and
// x86_64-elf's newlib, built with MISSING_SYSCALL_NAMES, ships none and calls fcntl itself.
int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    int const rc = fd_fcntl(fd, cmd, ap);
    va_end(ap);
    return rc;
}
}
