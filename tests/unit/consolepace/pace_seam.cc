// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Console test stubs. arch_console_write uses the buffered line-insert path.

#include <kickos/arch/arch.h>
#include <kickos/bench.h>
#include <kickos/console_tx.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

extern "C"
{
    int arch_console_write(char const* buf, size_t n)
    {
        return console_tx_insert_line(buf, n, KICKOS_CONSOLE_CRLF);
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
    // Stub lock sampling to avoid linking the scheduler and syscall table.
    constinit BenchLockRow g_bench_lock[KICKOS_KERNEL_CORES] = {};

    void bench_dist_add(uint32_t, BenchTick)
    {
    }

    void bench_lock_hold_add(BenchTick, void*)
    {
    }

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
