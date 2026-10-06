// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Publish-aware console write for freestanding diagnostic apps (no libc stdio, no heap).
//
// Sends through this thread's stdout cap at index 0 and falls back to the kernel debug
// console for the unsent remainder when index 0 is empty (-KOS_EBADF) or the driver
// has no receiver (-KOS_EAGAIN, -KOS_ECONNREFUSED). A receiver taking fewer bytes than a
// chunk, none included, is offered the rest again on the endpoint. kos_print alone is not
// enough: console_emit drops every byte handed to the kernel console once the UART is
// USER_OWNED (kernel/init/console.cc). The kernel console answers -KOS_EBUSY where a publish
// landed between the two and index 0 takes the line again, and the remainder is sent there.
//
// libc's _write (user/src/newlib_stubs.cc) and the TAP harness (tests/tap/tap.cc) write
// through stdout_write below.

#ifndef KICKOS_SYS_EMIT_H
#define KICKOS_SYS_EMIT_H

#include <kickos/console_tx.h>
#include <kickos/sys.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos
{

// 10 bits of wire per byte at 8N1, at the 115200 every chip console in the fleet programs.
// A console that degrades below it (sam3x8e falling back to its 4 MHz fast RC) waits short
// of its own wire time, and then reports the loss rather than hiding it.
constexpr uint64_t EMIT_WIRE_NS_PER_BYTE = 86806u;

// What a write spends waiting for the kernel console ring before it gives the remainder up:
// ONE FULL RING'S TRANSMISSION, 44.4 ms at the 512-byte default. Past that, every byte that
// was queued when the stall began has had time to leave, so a ring still refusing is not
// draining at all; and no one chunk ever needs more than a ring to fit. kprintf_paced
// (kernel/init/console.cc) bounds itself by the same reasoning in the currency it has,
// attempts that free a byte, which userspace cannot see.
//
// A YIELD COUNT CANNOT STAND IN FOR THIS: an idle single-core box spends 256 yields in far
// less than one byte time, which is what made the loss below routine and silent.
constexpr uint64_t EMIT_DRAIN_NS =
    static_cast<uint64_t>(KICKOS_CONSOLE_TX_SIZE) * EMIT_WIRE_NS_PER_BYTE;

namespace emit_detail
{

// Bytes the kernel console refused that no reader has been told about. Sticky on purpose:
// the console is the only way to say anything and it is the thing that was full, so a loss
// is announced on the first write that fits AFTER it, never at the point of it.
//
// PLAIN, because <kickos/sys/atomic.h> offers userspace no read-modify-write and armv6m has
// no instruction for one either. Two threads dropping at once can lose one of the two adds,
// so the count is a floor and not a tally; the marker still reaches the reader, which is
// what a verdict rests on.
inline uint32_t g_dropped = 0;

// `deadline` is 0 until a write has actually stalled, and is shared across every offer of
// one kconsole_write_all call, so a ring that never drains costs that call EMIT_DRAIN_NS
// once rather than once per offer. Any byte accepted clears it again: that is the proof the
// drain is alive. A real deadline never reads 0, kos_clock_now counting from boot.
//
// Stops at -KOS_EBUSY with `served` set: the rest belongs on this thread's stdout endpoint.
inline size_t offer(char const* s, size_t n, uint64_t& deadline, bool& served)
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
        if (w == -KOS_EAGAIN)
        {
            uint64_t const now = kos_clock_now();
            if (deadline == 0)
            {
                deadline = now + EMIT_DRAIN_NS;
            }
            else if (now >= deadline)
            {
                break;
            }
            kos_yield();
            continue;
        }
        if (w <= 0)
        {
            break; // a rejected buffer; no retry can change the answer
        }
        deadline = 0;
        sent += static_cast<size_t>(w);
    }
    return sent;
}

// Decimal of `v` into `out`, returning the digit count. There is no libc on this path: the
// header is included by the TAP harness and by libc's own _write.
inline size_t decimal(uint32_t v, char* out)
{
    char rev[10];
    size_t k = 0;
    do
    {
        rev[k] = static_cast<char>('0' + (v % 10u));
        k++;
        v = v / 10u;
    }
    while (v != 0u);
    for (size_t i = 0; i < k; i++)
    {
        out[i] = rev[k - 1u - i];
    }
    return k;
}

// The marker a reader refuses on (tests/integration/check_tap_stream.sh).
//
// IT OPENS WITH A NEWLINE because the write that was cut may have ended mid-line, and a
// marker landing on that line's tail is one no parse of the reader's finds. It is spelled
// as a TAP comment so that a harness stream carrying it still reconciles, and it is kept
// short so the ring that just refused a line can still take it.
inline void announce(uint64_t& deadline)
{
    if (g_dropped == 0u)
    {
        return;
    }
    char const head[] = "\n# console dropped ";
    char const tail[] = " byte(s)\n";
    char line[40];
    size_t k = 0;
    for (size_t i = 0; i + 1u < sizeof(head); i++)
    {
        line[k] = head[i];
        k++;
    }
    uint32_t const announced = g_dropped;
    k += decimal(announced, line + k);
    for (size_t i = 0; i + 1u < sizeof(tail); i++)
    {
        line[k] = tail[i];
        k++;
    }
    // SUBTRACT what was announced rather than clearing: a drop another thread recorded while
    // this marker was on the wire is still owed a marker of its own.
    bool served = false;
    if (offer(line, k, deadline, served) == k)
    {
        g_dropped -= announced;
    }
}

}

// The bytes of s the kernel console took. `served` is set where it refused the rest because
// this thread's stdout endpoint takes it now, and those bytes are not counted as dropped.
inline size_t kconsole_offer(char const* s, size_t n, bool& served)
{
    uint64_t deadline = 0;
    emit_detail::announce(deadline);
    size_t const sent = emit_detail::offer(s, n, deadline, served);
    if (not served)
    {
        emit_detail::g_dropped += static_cast<uint32_t>(n - sent);
    }
    return sent;
}

inline void stdout_write(char const* s, size_t total);

inline void kconsole_write_all(char const* s, size_t n)
{
    bool served = false;
    size_t const sent = kconsole_offer(s, n, served);
    if (served)
    {
        stdout_write(s + sent, n - sent);
    }
}

inline void stdout_write(char const* s, size_t total)
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
        // A count short of the chunk, zero included, leaves the rest to be offered again. That
        // is no spin: the rendezvous consumed the receive, so the next send parks until the
        // driver receives again.
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
            sent += kconsole_offer(s + sent, total - sent, served);
            if (not served)
            {
                return;
            }
            // Never a spin on one state: each pass back needs the send to have found no
            // receiver and the kernel console then to have found one.
            continue;
        }
        sent += static_cast<size_t>(r);
    }
}

inline void emit(char const* s)
{
    size_t total = 0;
    while (s[total] != '\0')
    {
        total++;
    }
    stdout_write(s, total);
}

}

#endif
