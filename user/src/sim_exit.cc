// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The sim rides the host libc, whose exit() ends the PROCESS: a non-root thread calling
// it would take the whole system down instead of its own thread, and the buffered console
// ring would go out undrained. This routes exit() onto the same KOS_SYS_EXIT dispatch
// newlib_stubs.cc's _exit reaches on the cross ports. Compiled for the sim only.
//
// Overriding exit() and not _exit(): glibc's exit() calls its own hidden alias, so an
// _exit definition here would never be reached.

#include <kickos/sys.h>

#include <stdlib.h>

// Pulled from the archive by the app's own reference to exit(). Not force-linked: an
// image that never calls exit() has nothing to route.
extern "C" void exit(int code) noexcept
{
    kos_exit(code);
    while (true)
    {
    }
}
