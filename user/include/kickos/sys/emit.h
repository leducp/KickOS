// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The one stdout writer: kos_print (user/src/print.cc), libc's _write (user/src/newlib_stubs.cc)
// and the TAP harness (tests/tap/tap.cc) all write through stdout_write below. An app calls
// kos_print; the names here are its implementation.
//
// Sends through this thread's stdout cap at index 0, a local console or another node's service,
// and falls back to the kernel debug console for the unsent remainder only where no local route
// serves the send: index 0 empty (-KOS_EBADF), or nothing serving the published console
// (-KOS_ECONNREFUSED), a driver's task that ended or its endpoint gone. A far endpoint answers
// neither. Any other refusal ends the write. A receiver taking fewer bytes than a chunk, none
// included, is offered the rest again on the endpoint. The kernel console alone is not enough:
// console_emit drops every byte handed to it once the UART is USER_OWNED
// (kernel/init/console.cc). It answers -KOS_EBUSY where a publish landed
// between the two and index 0 takes the line again, and the remainder is sent there. A task
// that set O_NONBLOCK (stdout_nonblock) stops where either would wait, with -KOS_EAGAIN.

#ifndef KICKOS_SYS_EMIT_H
#define KICKOS_SYS_EMIT_H

#include <kickos/sys.h>
#include <kickos/sys/atomic.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos
{

// This task's O_NONBLOCK on fds 0 to 2, set by fcntl (user/src/newlib_fcntl.cc). One word per
// image, so a sibling task's flag is never this one.
inline Atomic<uint32_t, Order::RELAXED>& stdout_nonblock()
{
    static Atomic<uint32_t, Order::RELAXED> flag{0u};
    return flag;
}

// This task's console fds still open, bit n for fd n of 0 to 2: fd 0 reads, fds 1 and 2 write.
// close() clears a bit and nothing sets one again, since no call opens an fd. One word per
// image, as stdout_nonblock.
inline uint32_t& console_fds()
{
    static uint32_t fds = 0x7u;
    return fds;
}

inline bool console_fd_open(int fd)
{
    if (fd < 0 or fd > 2)
    {
        return false;
    }
    return (__atomic_load_n(&console_fds(), __ATOMIC_ACQUIRE) & (1u << fd)) != 0u;
}

// The bytes a write took, and where that is short of them all, the kernel's answer that stopped
// it; error is 0 once every byte went.
struct WriteResult
{
    size_t sent;
    int32_t error;
};

// The bytes of s the kernel console took, which waits in the kernel for room where this task
// blocks. Where it stopped short, error is -KOS_EBUSY where this thread's stdout endpoint takes the
// rest now, -KOS_EAGAIN where this task is non-blocking and the console would make it wait, and
// otherwise the refusal: a buffer the kernel refuses, a wait the writer's own cancellation ended.
inline WriteResult kconsole_offer(char const* s, size_t n)
{
    WriteResult r = {0, 0};
    uint32_t flags = 0u;
    if (stdout_nonblock() != 0u)
    {
        flags = KOS_KCONSOLE_NOWAIT;
    }
    while (r.sent < n)
    {
        int32_t const w = KconsoleRaw::write(s + r.sent, n - r.sent, flags);
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

// One send of the console's chunk. Out of the loop below: GCC keeps a 64-bit deadline live across
// a loop in a VFP register on an FP core, which would make every printing thread FP-live.
__attribute__((noinline)) inline int32_t stdout_send(char const* s, size_t n, bool nonblock)
{
    kos_monotonic_t until = KOS_NEVER;
    if (nonblock)
    {
        until = KOS_POLL;
    }
    return kos_send(0, s, n, until);
}

// The bytes of s taken, all of them where error is 0. Short with -KOS_EAGAIN where this task
// is non-blocking and the console would make it wait, else with the error that refused the rest:
// bytes a refused buffer or a cancelled writer loses are not counted.
inline WriteResult stdout_write(char const* s, size_t total)
{
    size_t sent = 0;
    bool const nonblock = stdout_nonblock() != 0u;
    while (sent < total)
    {
        size_t chunk = total - sent;
        if (chunk > KOS_EP_MSG_MAX)
        {
            chunk = KOS_EP_MSG_MAX;
        }
        int32_t const r = stdout_send(s + sent, chunk, nonblock);
        // A count short of the chunk, zero included, leaves the rest to be offered again. That
        // is no spin: the rendezvous consumed the receive, so the next send parks until the
        // driver receives again, or for a non-blocking task answers at once.
        if (r >= 0)
        {
            sent += static_cast<size_t>(r);
            continue;
        }
        if (r != -KOS_EBADF and r != -KOS_ECONNREFUSED)
        {
            return {sent, r};
        }
        // Remainder only: resending from the start duplicates the chunks the driver already
        // took.
        WriteResult const k = kconsole_offer(s + sent, total - sent);
        sent += k.sent;
        // Never a spin on one state: each pass back needs the send to have found the console
        // unserved and the kernel console then to have found it served.
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
