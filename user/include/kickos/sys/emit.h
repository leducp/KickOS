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
inline size_t offer(char const* s, size_t n, uint64_t& deadline)
{
    size_t sent = 0;
    while (sent < n)
    {
        int32_t const w = kos_kconsole_write(s + sent, n - sent);
        if (w < 0)
        {
            break; // a rejected buffer; no retry can change the answer
        }
        if (w == 0)
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
    if (offer(line, k, deadline) == k)
    {
        g_dropped -= announced;
    }
}

}

inline void kconsole_write_all(char const* s, size_t n)
{
    uint64_t deadline = 0;
    emit_detail::announce(deadline);
    size_t const sent = emit_detail::offer(s, n, deadline);
    emit_detail::g_dropped += static_cast<uint32_t>(n - sent);
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
