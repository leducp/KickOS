// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// newlib's fcntl for the console's fds. NOT compiled for the sim, where host glibc provides it.

#include <kickos/sys.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>

namespace
{
    int refused(int rc)
    {
        errno = EINVAL;
        if (rc == -KOS_ENOSYS)
        {
            errno = ENOSYS;
        }
        return -1;
    }

    // fds 0 to 2 share one description, the console, whose O_NONBLOCK is the calling task's
    // (kos_task_nonblock).
    int fd_fcntl(int fd, int cmd, va_list ap)
    {
        if (fd < 0 or fd > 2)
        {
            errno = EBADF;
            return -1;
        }
        switch (cmd)
        {
            case F_GETFL:
            {
                int const nonblock = kos_task_nonblock(KOS_NONBLOCK_GET);
                if (nonblock < 0)
                {
                    return refused(nonblock);
                }
                int access = O_WRONLY;
                if (fd == 0)
                {
                    access = O_RDONLY;
                }
                if (nonblock > 0)
                {
                    return access | O_NONBLOCK;
                }
                return access;
            }
            case F_SETFL:
            {
                int op = KOS_NONBLOCK_CLEAR;
                if ((va_arg(ap, int) & O_NONBLOCK) != 0)
                {
                    op = KOS_NONBLOCK_SET;
                }
                int const rc = kos_task_nonblock(op);
                if (rc < 0)
                {
                    return refused(rc);
                }
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
