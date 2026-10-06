// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_CONSOLE_RETRY_H
#define KICKOS_ARCH_CONSOLE_RETRY_H

#include <stddef.h>

namespace kickos::console_retry
{
    // A polled AMP UART can lose its claim after sending CR but before sending LF. The syscall
    // reports only complete input bytes, so remember that CR for the caller's next offer.
    // `put` returns true only when it stored one wire byte under a live claim.
    template<class Put>
    size_t write(char const* buf, size_t n, bool* cr_pending, Put put)
    {
        if (n == 0)
        {
            return 0;
        }
        size_t done = 0;
        if (cr_pending != nullptr and *cr_pending)
        {
            if (not put('\n'))
            {
                return 0;
            }
            *cr_pending = false;
            // The usual next offer starts with the newline whose CR was already sent. If a
            // caller ignored the short count, finish that old line before its new bytes.
            if (buf[0] == '\n')
            {
                done = 1;
            }
        }
        for (; done < n; done++)
        {
#if KICKOS_CONSOLE_CRLF
            if (buf[done] == '\n')
            {
                if (not put('\r'))
                {
                    break;
                }
                if (cr_pending != nullptr)
                {
                    *cr_pending = true;
                }
            }
#endif
            if (not put(buf[done]))
            {
                break;
            }
            if (cr_pending != nullptr and buf[done] == '\n')
            {
                *cr_pending = false;
            }
        }
        return done;
    }

    enum class Put
    {
        STORED,
        LOST,   // the claim ended or could not be had
        WEDGED, // the FIFO never drained
    };

    // Panic, fault and ISR output on a claimed UART, which no caller can offer again. A line
    // whose claim was lost, or whose FIFO wedged, is dropped up to and including its '\n',
    // however many calls carry it.
    class PolledLine
    {
    public:
        // False once `put` reports a wedged FIFO.
        template<class PutFn>
        bool write(char const* buf, size_t n, PutFn put)
        {
            for (size_t i = 0; i < n; i++)
            {
                char const c = buf[i];
                if (dropping_)
                {
                    dropping_ = c != '\n';
                    continue;
                }
                Put const result = put(c);
                dropping_ = result != Put::STORED and c != '\n';
                if (result == Put::WEDGED)
                {
                    return false;
                }
            }
            return true;
        }

    private:
        bool dropping_ = false;
    };
}

#endif
