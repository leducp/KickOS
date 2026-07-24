// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The default init's pin-mux step: apply the selected board pin map (see
// <kickos/sys/pinmap.h>) before the console bring-up -- the DAG middle of the
// clock->pinmux->console->app chain. A board with an empty map (count = 0) is a
// no-op. On the first failing entry it reports port/pin/rc and returns that rc.
//
// HARD RULE (as in console_bringup.cc): no libc stdio here. Diagnostics go via
// kos::print (the RTT / kernel debug path), never stdio.

#include <kickos/sys/pinmap.h>
#include <kickos/sys/init.h>
#include <kickos/sys.h> // kos_pinmux_set
#include <kickos/kos.h> // kos::print (RTT/kernel debug path; NOT stdio)

namespace
{
    // Decimal-format a non-negative value into buf's tail; return the first digit.
    char const* u32_dec(uint32_t v, char* buf, size_t n)
    {
        size_t i = n;
        buf[--i] = '\0';
        do
        {
            buf[--i] = static_cast<char>('0' + (v % 10u));
            v /= 10u;
        } while (v != 0u and i != 0);
        return &buf[i];
    }
}

extern "C" int kickos_pinmux_run(void)
{
    for (uint32_t i = 0; i < kickos_board_pinmap.count; ++i)
    {
        struct kos_pinmux_entry const* e = &kickos_board_pinmap.entries[i];
        int const rc = kos_pinmux_set(e->port, e->pin, e->func);
        if (rc != 0)
        {
            char buf[16];
            kos::print("[pinmux] ERROR: port ");
            kos::print(u32_dec(e->port, buf, sizeof(buf)));
            kos::print(" pin ");
            kos::print(u32_dec(e->pin, buf, sizeof(buf)));
            kos::print(" rc -");
            kos::print(u32_dec(static_cast<uint32_t>(-rc), buf, sizeof(buf)));
            kos::print("\n");
            return rc;
        }
    }
    return 0;
}
