// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The one stdout writer: kos_print (user/src/print.cc), libc's _write (user/src/newlib_stubs.cc)
// and the TAP harness (tests/tap/tap.cc) all write through stdout_write below. An app calls
// kos_print; the names here are its implementation.
//
// Sends through this thread's stdout cap at index 0 and falls back to the kernel debug
// console for the unsent remainder only where no route serves the send: index 0 empty
// (-KOS_EBADF), or nothing serving the published console (-KOS_EAGAIN, -KOS_ECONNREFUSED), a
// driver's task that ended or its endpoint gone. Any other refusal ends the write. A receiver
// taking fewer bytes than a chunk, none included, is offered the rest again on the endpoint.
// The kernel console alone is not enough: console_emit drops every byte handed to it once the
// UART is USER_OWNED (kernel/init/console.cc). It answers -KOS_EBUSY where a publish landed
// between the two and index 0 takes the line again, and the remainder is sent there. A task
// that set O_NONBLOCK is answered -KOS_ETIMEDOUT by either where it would wait, and the write
// stops there.

#ifndef KICKOS_SYS_EMIT_H
#define KICKOS_SYS_EMIT_H

#include <kickos/sys.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos
{

// The bytes a write took, and where that is short of them all, the kernel's answer that stopped
// it; error is 0 once every byte went.
struct WriteResult
{
    size_t sent;
    int32_t error;
};

// The bytes of s the kernel console took, which waits in the kernel for room where this task
// blocks. Where it stopped short, error is -KOS_EBUSY where this thread's stdout endpoint takes the
// rest now, -KOS_ETIMEDOUT where this task is non-blocking and the console would make it wait, and
// otherwise the refusal: a buffer the kernel refuses, a wait the writer's own cancellation ended.
inline WriteResult kconsole_offer(char const* s, size_t n)
{
    WriteResult r = {0, 0};
    while (r.sent < n)
    {
        int32_t const w = kos_kconsole_write(s + r.sent, n - r.sent);
        if (w < 0)
        {
            r.error = w;
            break;
        }
        // The kernel never answers 0 for bytes it was offered; a stop still carries an error.
        if (w == 0)
        {
            r.error = -KOS_EIO;
            break;
        }
        r.sent += static_cast<size_t>(w);
    }
    return r;
}

inline WriteResult stdout_write(char const* s, size_t total);

inline void kconsole_write_all(char const* s, size_t n)
{
    WriteResult const r = kconsole_offer(s, n);
    if (r.error == -KOS_EBUSY)
    {
        (void)stdout_write(s + r.sent, n - r.sent);
    }
}

// The bytes of s taken, all of them where error is 0. Short with -KOS_ETIMEDOUT where this task
// is non-blocking and the console would make it wait, else with the error that refused the rest:
// bytes a refused buffer or a cancelled writer loses are not counted.
inline WriteResult stdout_write(char const* s, size_t total)
{
    size_t sent = 0;
    while (sent < total)
    {
        size_t chunk = total - sent;
        if (chunk > KOS_EP_MSG_MAX)
        {
            chunk = KOS_EP_MSG_MAX;
        }
        int32_t const r = kos_send(0, s + sent, chunk);
        // A count short of the chunk, zero included, leaves the rest to be offered again. That
        // is no spin: the rendezvous consumed the receive, so the next send parks until the
        // driver receives again, or for a non-blocking task answers at once.
        if (r >= 0)
        {
            sent += static_cast<size_t>(r);
            continue;
        }
        if (r != -KOS_EBADF and r != -KOS_EAGAIN and r != -KOS_ECONNREFUSED)
        {
            return {sent, r};
        }
        // THE PEER CLOSING DOES NOT FREE THIS SIDE. -KOS_ECONNREFUSED means the driver died and
        // nothing may restart it, and this cap is now the only thing pinning its endpoint slot,
        // so close it or the slot is stranded for this task's whole life. -KOS_EAGAIN keeps the
        // cap: a restarted driver serves it again. -KOS_EBADF is pre-publish: index 0 is empty
        // and there is nothing to close.
        if (r == -KOS_ECONNREFUSED)
        {
            (void)kos_handle_close(KOS_CAP_STDOUT);
        }
        // Remainder only: resending from the start duplicates the chunks the driver already
        // took.
        WriteResult const k = kconsole_offer(s + sent, total - sent);
        sent += k.sent;
        // Never a spin on one state: each pass back needs the send to have found no receiver
        // and the kernel console then to have found one.
        if (k.error != -KOS_EBUSY)
        {
            return {sent, k.error};
        }
    }
    return {sent, 0};
}

inline void emit(char const* s)
{
    size_t total = 0;
    while (s[total] != '\0')
    {
        total++;
    }
    (void)stdout_write(s, total);
}

}

#endif
