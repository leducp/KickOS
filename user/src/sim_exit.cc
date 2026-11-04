// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The sim rides the host libc, whose exit() ends the host PROCESS, the whole system with it, and
// leaves the buffered console ring undrained. This routes exit() onto KOS_SYS_EXIT's task scope,
// as newlib_stubs.cc's _exit does on the cross ports. Compiled for the sim only.
//
// Overriding exit() and not _exit(): glibc's exit() calls its own hidden alias, so an
// _exit definition here would never be reached.

#include <kickos/arch/arch.h>
#include <kickos/sys.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Pulled from the archive by the app's own reference to exit(). Not force-linked: an
// image that never calls exit() has nothing to route.
extern "C" void exit(int code) noexcept
{
    // The host's atexit handlers are the host process's, so only the streams are flushed.
    fflush(nullptr);
    arch_syscall(KOS_SYS_EXIT, static_cast<uintptr_t>(code), KOS_EXIT_TASK, 0, 0);
    while (true)
    {
    }
}
