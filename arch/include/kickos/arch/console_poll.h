// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_CONSOLE_POLL_H
#define KICKOS_ARCH_CONSOLE_POLL_H

#include <kickos/config/limits.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos::console_poll
{
    // A chip's arch_console_write_sync over its TX edge: each byte waits for SlotFree to answer
    // nonzero, at most KICKOS_POLL_SPIN_MAX reads, then goes to Push. The first wait that runs
    // out drops the rest and answers false. The two functions are template arguments so each call
    // is direct: the trap red-zone gate needs a record for every indirect one.
    template<int (*SlotFree)(void), void (*Push)(uint8_t)>
    __attribute__((always_inline)) inline bool write(char const* buf, size_t n)
    {
        for (size_t i = 0; i < n; i++)
        {
            uint32_t spin = 0;
            while (SlotFree() == 0)
            {
                spin++;
                if (spin > KICKOS_POLL_SPIN_MAX)
                {
                    return false;
                }
            }
            Push(static_cast<uint8_t>(buf[i]));
        }
        return true;
    }
}

#endif
