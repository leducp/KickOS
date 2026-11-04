// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The POSIX calls the C++ library asks of its system beside the threads layer. Every reference to
// sysconf reaches this one through --wrap, never the one some newlibs carry.

#include <kickos/sys.h>

#include <errno.h>
#include <sched.h>
#include <unistd.h>

extern "C" long __wrap_sysconf(int name);

extern "C"
{

int sched_yield(void)
{
    kos_yield();
    return 0;
}

// The cores are the calling task's; the page size is the one newlib's malloc asks for.
long __wrap_sysconf(int name)
{
    switch (name)
    {
        case _SC_NPROCESSORS_CONF:
        case _SC_NPROCESSORS_ONLN:
        {
            int const mask = kos_task_cores(KOS_TASK_NONE);
            if (mask <= 0)
            {
                return 1;
            }
            long cores = 0;
            for (uint32_t m = static_cast<uint32_t>(mask); m != 0u; m &= m - 1u)
            {
                cores++;
            }
            return cores;
        }
        case _SC_PAGESIZE:
        {
            return 4096;
        }
        default:
        {
            errno = EINVAL;
            return -1;
        }
    }
}
}
