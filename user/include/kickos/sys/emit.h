// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Publish-aware console write for freestanding diagnostic apps (no libc stdio, no heap).
//
// Sends through this thread's stdout cap at index 0 and falls back to the kernel debug
// console for the unsent remainder when index 0 is empty (-KOS_EBADF) or the driver
// died (-KOS_EPIPE). kos_print alone is not enough: console_emit drops every byte
// handed to the kernel console once the UART is USER_OWNED (kernel/init/console.cc).
//
// The same policy exists in tests/tap/tap.cc emit() and libc's _write
// (user/src/newlib_stubs.cc); keep the three in step.

#ifndef KICKOS_SYS_EMIT_H
#define KICKOS_SYS_EMIT_H

#include <kickos/sys.h>

#include <stddef.h>

namespace kickos
{

inline void emit(char const* s)
{
    size_t total = 0;
    while (s[total] != '\0')
    {
        total++;
    }
    size_t sent = 0;
    while (sent < total)
    {
        size_t chunk = total - sent;
        if (chunk > KOS_EP_MSG_MAX)
        {
            chunk = KOS_EP_MSG_MAX;
        }
        long const r = kos_send(0, s + sent, chunk);
        // r == 0 (a receiver with no buffer) would spin forever: fall back, don't retry.
        if (r <= 0)
        {
            // Remainder only: resending from the start duplicates the chunks the driver
            // already took.
            kos_kconsole_write(s + sent, total - sent);
            return;
        }
        sent += static_cast<size_t>(r);
    }
}

} // namespace kickos

#endif
