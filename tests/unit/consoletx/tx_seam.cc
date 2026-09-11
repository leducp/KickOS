// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What console_tx.cc leaves undefined once tests/unit/consoleseam answers the transport:
// the ownership boundary kernel/init/console.cc owns, which this gate does not compile.

#include <kickos/irq.h>

extern "C"
{
    // Both ownership reads are pinned kernel-owned: this seam measures the RING, and the
    // publish sequence is gated in tests/unit/consoleown/publish_handoff.cc.
    int console_owner_is_kernel(void)
    {
        return 1;
    }

    int console_chip_writable(void)
    {
        return 1;
    }

    // NOT the real reference count: nothing here reads it back, so no arm of this gate may
    // claim anything about publish-drain convergence.
    void console_chip_writer_enter(void)
    {
    }

    void console_chip_writer_leave(void)
    {
    }

    // console.cc's. The insert reaches it for the UNARMED ring alone, where there is no ring
    // to interleave with, so this suite needs only the symbol.
    void console_write_line_sync(char const*, size_t)
    {
    }
}

namespace kickos
{
    void kpanic(char const*) __attribute__((noreturn));
    void kpanic(char const*)
    {
        __builtin_trap();
    }
}
