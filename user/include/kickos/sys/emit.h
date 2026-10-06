// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The one stdout writer: kos_print (user/src/print.cc), libc's _write (user/src/newlib_stubs.cc)
// and the TAP harness (tests/tap/tap.cc) all write through stdout_write below. An app calls
// kos_print; the names here are its implementation.
//
// Sends through this thread's stdout cap at index 0 and falls back to the kernel debug
// console for the unsent remainder when index 0 is empty (-KOS_EBADF), or nothing serves the
// published console (-KOS_EAGAIN, -KOS_ECONNREFUSED): a driver's task that ended, or its
// endpoint gone. A receiver taking fewer bytes than a chunk, none included, is offered the rest
// again on the endpoint. The kernel console alone is not enough: console_emit drops every byte
// handed to it once the UART is USER_OWNED (kernel/init/console.cc). It answers -KOS_EBUSY where
// a publish landed between the two and index 0 takes the line again, and the remainder is sent
// there. A task that set O_NONBLOCK is answered -KOS_ETIMEDOUT by either where it would wait,
// and the write stops there.

#ifndef KICKOS_SYS_EMIT_H
#define KICKOS_SYS_EMIT_H

#include <kickos/sys.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos
{

// The bytes of s the kernel console took. A refusal with nothing taken is offered again: the ring
// drains and a peer's claim on a shared UART ends. `served` is set where it refused the rest
// because this thread's stdout endpoint takes it now, `would_block` where this task is
// non-blocking and the console would make it wait. It also stops at a buffer the kernel refuses,
// and at a wait the writer's own cancellation ended.
inline size_t kconsole_offer(char const* s, size_t n, bool& served, bool& would_block)
{
    size_t sent = 0;
    while (sent < n)
    {
        int32_t const w = kos_kconsole_write(s + sent, n - sent);
        if (w == -KOS_EBUSY)
        {
            served = true;
            break;
        }
        if (w == -KOS_ETIMEDOUT)
        {
            would_block = true;
            break;
        }
        if (w == -KOS_EAGAIN)
        {
            kos_yield();
            continue;
        }
        if (w <= 0)
        {
            break;
        }
        sent += static_cast<size_t>(w);
    }
    return sent;
}

inline size_t stdout_write(char const* s, size_t total);

inline void kconsole_write_all(char const* s, size_t n)
{
    bool served = false;
    bool would_block = false;
    size_t const sent = kconsole_offer(s, n, served, would_block);
    if (served)
    {
        (void)stdout_write(s + sent, n - sent);
    }
}

// The bytes of s taken: all of them, short only where this task is non-blocking and the console
// would make it wait. Bytes a cancelled writer or a refused buffer loses count as taken.
inline size_t stdout_write(char const* s, size_t total)
{
    size_t sent = 0;
    while (sent < total)
    {
        size_t chunk = total - sent;
        if (chunk > KOS_EP_MSG_MAX)
        {
            chunk = KOS_EP_MSG_MAX;
        }
        long const r = kos_send(0, s + sent, chunk);
        if (r == -KOS_ETIMEDOUT)
        {
            return sent;
        }
        // A count short of the chunk, zero included, leaves the rest to be offered again. That
        // is no spin: the rendezvous consumed the receive, so the next send parks until the
        // driver receives again, or for a non-blocking task answers at once.
        if (r < 0)
        {
            // THE PEER CLOSING DOES NOT FREE THIS SIDE. -KOS_ECONNREFUSED means the driver
            // died and nothing may restart it, and this cap is now the only thing pinning its
            // endpoint slot, so close it or the slot is stranded for this task's whole life.
            // -KOS_EAGAIN keeps the cap: a restarted driver serves it again. -KOS_EBADF is
            // pre-publish: index 0 is empty and there is nothing to close.
            if (r == -KOS_ECONNREFUSED)
            {
                (void)kos_handle_close(KOS_CAP_STDOUT);
            }
            // Remainder only: resending from the start duplicates the chunks the driver
            // already took.
            bool served = false;
            bool would_block = false;
            size_t const took = kconsole_offer(s + sent, total - sent, served, would_block);
            sent += took;
            if (would_block)
            {
                return sent;
            }
            if (not served)
            {
                return total;
            }
            // Never a spin on one state: each pass back needs the send to have found no
            // receiver and the kernel console then to have found one.
            continue;
        }
        sent += static_cast<size_t>(r);
    }
    return sent;
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
