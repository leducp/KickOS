// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What kernel/init/console.cc leaves undefined when it is compiled WITHOUT console_tx.cc and
// without a kernel behind it, apart from what a gate records: the device writes, the reclaim,
// the ring's arm, the window, the dark wait, the console's server and the current thread.

#include <kickos/arch/arch.h>
#include <kickos/console_tx.h>
#include <kickos/kernel.h>
#include <kickos/sys/errno.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

extern "C"
{
    int arch_in_isr(void) { return 0; }

    void arch_console_flush_sync(void) {}
    void arch_console_reclaim_window(uintptr_t* base, size_t* size)
    {
        *base = 0x40000000u;
        *size = 0x100u;
    }

    int console_tx_insert_record_line(char const*, size_t, int) { return 0; }
    void console_tx_flush_sync(void) {}
    int console_held_append(uint32_t, char const*, uint32_t, uint32_t) { return 0; }
    int console_held_commit(uint32_t) { return 0; }
    void console_held_abandon(uint32_t) {}
    void console_held_write_sync(void) {}
    void console_held_clear(void) {}
    uint32_t console_held_ready(void) { return 0; }
    char const* console_held_data(void) { return nullptr; }
    void console_held_take(uint32_t) {}

    int kvsnprintf(char* buf, size_t size, char const* fmt, va_list ap)
    {
        return vsnprintf(buf, size, fmt, ap);
    }

    void arch_shutdown(int status)
    {
        printf("FIXTURE FAIL: arch_shutdown(%d) ended the arm\n", status);
        exit(1);
    }
    int arch_reboot(void) { return 0; }
    void kfault_terminate(void)
    {
        printf("FIXTURE FAIL: kfault_terminate ended the arm\n");
        exit(1);
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
    bool dev_window_held_outside(uintptr_t, size_t, Task const*) { return false; }
    bool task_serves_console(Task const*) { return false; }

    // No ring to wait on: the write ends.
    int console_room_wait(char const*, size_t, int) { return -KOS_ECANCELED; }

    void cap_console_deliver() {}
}
