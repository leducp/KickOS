// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What console.cc and console_tx.cc leave undefined once tests/unit/consoleseam answers the
// transport. The ownership state and the writer count are the REAL ones here, so the
// handover protocol runs as it ships.

#include <kickos/arch/arch.h>
#include <kickos/console_tx.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

extern "C"
{
    // Every buffered chip's arch_console_write is exactly this, so the routing decision in
    // console.cc reaches the real ring producer.
    int arch_console_write(char const* buf, size_t n)
    {
        console_tx_write(buf, n);
        return 1;
    }

    void arch_console_flush_sync(void)
    {
    }

    void arch_console_reclaim(void)
    {
    }

    void arch_console_reclaim_window(uintptr_t* base, size_t* size)
    {
        *base = 0x40000000u;
        *size = 0x100u;
    }

    int kvsnprintf(char* buf, size_t size, char const* fmt, va_list ap)
    {
        return vsnprintf(buf, size, fmt, ap);
    }

    // Distinct statuses: an arm that expects the panic path must be able to tell it from an
    // ordinary failed expectation.
    void arch_shutdown(int status)
    {
        printf("SEAM: arch_shutdown(%d)\n", status);
        exit(43);
    }

    int arch_reboot(void)
    {
        return 0;
    }

    void kfault_terminate(void)
    {
        printf("SEAM: kfault_terminate\n");
        exit(42);
    }
    // No stack switch, as on ARCH_SIM: a host thread stack is megabytes and no red-zone
    // class measures one.
    void kickos_panic_stack_enter(char const* msg, char const* file, unsigned line,
                                  uintptr_t top)
    {
        (void)top;
        kickos_panic_report(msg, file, line);
    }
}

namespace kickos
{
    bool dev_window_free(uintptr_t, size_t)
    {
        return true;
    }

    int32_t cap_console_deliver(char const*, size_t)
    {
        return 0;
    }

    namespace sched
    {
        Thread* current()
        {
            return nullptr;
        }
    }
}
