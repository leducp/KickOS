// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What console.cc and console_tx.cc leave undefined once tests/unit/consoleseam answers the
// transport. The ownership state, the writer count and the held store are the REAL ones.

#include <kickos/arch/arch.h>
#include <kickos/console_tx.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "held_seam.h"

namespace heldseam
{
    uint32_t g_deliveries = 0;
    kickos::Thread* g_current = nullptr;
    bool g_window_free = true;
    bool g_serves_console = false;
    uint32_t g_reclaims = 0;
    uint32_t g_dark_wakes = 0;
    int (*g_dark_wait)(void) = nullptr;
    void (*g_flush_hook)(void) = nullptr;
}

extern "C"
{
    int arch_console_write(char const* buf, size_t n)
    {
        return console_tx_insert_line(buf, n, 0);
    }

    void arch_console_flush_sync(void)
    {
        void (*const hook)(void) = heldseam::g_flush_hook;
        heldseam::g_flush_hook = nullptr;
        if (hook != nullptr)
        {
            hook();
        }
    }

    void arch_console_reclaim(void)
    {
        heldseam::g_reclaims = heldseam::g_reclaims + 1;
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
        return heldseam::g_window_free;
    }

    bool dev_window_held_outside(uintptr_t, size_t, Task const*)
    {
        return false;
    }

    bool task_serves_console(Task const*)
    {
        return heldseam::g_serves_console;
    }

    int console_dark_wait(void)
    {
        if (heldseam::g_dark_wait != nullptr)
        {
            return heldseam::g_dark_wait();
        }
        return 0;
    }

    void console_dark_wake(void)
    {
        heldseam::g_dark_wakes = heldseam::g_dark_wakes + 1;
    }

    // No receiver is ever parked here: what a record leaves behind is what the held store says.
    void cap_console_deliver()
    {
        heldseam::g_deliveries = heldseam::g_deliveries + 1;
    }

    bool cap_console_serves(Thread const*)
    {
        return false;
    }

    namespace sched
    {
        Thread* current()
        {
            return heldseam::g_current;
        }
    }
}
