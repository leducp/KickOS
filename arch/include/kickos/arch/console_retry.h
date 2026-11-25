// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_CONSOLE_RETRY_H
#define KICKOS_ARCH_CONSOLE_RETRY_H

#include <stddef.h>

#include <kickos/arch/arch.h>

namespace kickos::console_retry
{
    // The bytes of buf whose wire bytes `put` stored, stopping at the first it refused.
    template<class Put>
    size_t write(char const* buf, size_t n, Put put)
    {
        size_t done = 0;
        for (; done < n; done++)
        {
#if KICKOS_CONSOLE_CRLF
            if (buf[done] == '\n' and not put('\r'))
            {
                break;
            }
#endif
            if (not put(buf[done]))
            {
                break;
            }
        }
        return done;
    }

    // write, each wire byte its own masked span: `put` runs masked and interrupts are open
    // between two bytes, so a line keeps a local interrupt waiting for one byte at most.
    template<class Put>
    size_t write_masked(char const* buf, size_t n, Put put)
    {
        return write(buf, n, [&put](char c) {
            arch_irq_state_t const irq = arch_irq_save();
            bool const stored = put(c);
            arch_irq_restore(irq);
            return stored;
        });
    }
}

#endif
