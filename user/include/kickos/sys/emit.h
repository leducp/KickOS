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

// Yields spent waiting for the console TX ring to drain before a line is given up.
constexpr unsigned EMIT_DRAIN_YIELDS = 256;

inline void kconsole_write_all(char const* s, size_t n)
{
    size_t sent = 0;
    unsigned stalls = 0;
    while (sent < n)
    {
        int32_t const w = kos_kconsole_write(s + sent, n - sent);
        if (w < 0)
        {
            return; // a rejected buffer; no retry can change the answer
        }
        if (w == 0)
        {
            if (stalls >= EMIT_DRAIN_YIELDS)
            {
                return;
            }
            stalls++;
            kos_yield();
            continue;
        }
        stalls = 0;
        sent += static_cast<size_t>(w);
    }
}

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
            // THE PEER CLOSING DOES NOT FREE THIS SIDE. -KOS_EPIPE means the driver died and
            // this cap is now the only thing pinning its endpoint slot, so close it or the
            // slot is stranded for this task's whole life. -KOS_EBADF is pre-publish: index 0
            // is empty and there is nothing to close.
            if (r == -KOS_EPIPE)
            {
                (void)kos_handle_close(KOS_CAP_STDOUT);
            }
            // Remainder only: resending from the start duplicates the chunks the driver
            // already took.
            kconsole_write_all(s + sent, total - sent);
            return;
        }
        sent += static_cast<size_t>(r);
    }
}

}

#endif
