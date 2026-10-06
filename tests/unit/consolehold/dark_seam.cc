// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The arch console and the device window under the REAL kernel/init/console.cc and
// kernel/init/console_tx.cc, for the K-seam built with KSEAM_REAL_CONSOLE: a polled device whose
// bytes are recorded, a reclaim that is counted, and a register window held until a test frees it.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include <string>

#include <kickos/arch/arch.h>
#include <kickos/console_tx.h>
#include <kickos/kernel.h>

#include "dark_seam.h"

namespace darkseam
{
    std::string g_wire;
    uint32_t g_reclaims = 0;
    bool g_window_free = true;

    void reset()
    {
        g_wire.clear();
        g_reclaims = 0;
        g_window_free = true;
    }
}

extern "C"
{
    int arch_console_write(char const* buf, size_t n)
    {
        darkseam::g_wire.append(buf, n);
        return static_cast<int>(n);
    }

    void arch_console_write_sync(char const* buf, size_t n)
    {
        darkseam::g_wire.append(buf, n);
    }

    void arch_console_flush_sync(void)
    {
    }

    void arch_console_reclaim(void)
    {
        darkseam::g_reclaims = darkseam::g_reclaims + 1;
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

    struct console_tx_backend const* arch_console_tx_backend(char**, uint32_t*, int*)
    {
        return nullptr;
    }

    void arch_shutdown(int status)
    {
        printf("FIXTURE FAIL: arch_shutdown(%d) ended the arm\n", status);
        exit(1);
    }

    int arch_reboot(void)
    {
        return 0;
    }

    void kfault_terminate(void)
    {
        printf("FIXTURE FAIL: a panic ended the arm\n");
        fflush(stdout);
        exit(1);
    }

    void kickos_panic_stack_enter(char const* msg, char const* file, unsigned line, uintptr_t)
    {
        kickos_panic_report(msg, file, line);
    }
}

namespace kickos
{
    bool dev_window_free(uintptr_t, size_t)
    {
        return darkseam::g_window_free;
    }

    bool dev_window_held_outside(uintptr_t, size_t, Task const*)
    {
        return false;
    }
}
