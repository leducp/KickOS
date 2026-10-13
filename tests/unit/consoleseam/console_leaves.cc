// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What kernel/init/console.cc leaves undefined that every gate compiling it answers alike. Each
// terminal path exits with its own status, so an arm expecting a panic can tell it from a failed
// expectation.

#include <stdio.h>
#include <stdlib.h>

#include <kickos/arch/arch.h>
#include <kickos/kernel.h>

extern "C"
{
    void arch_console_reclaim_window(uintptr_t* base, size_t* size)
    {
        *base = 0x40000000u;
        *size = 0x100u;
    }

    void arch_shutdown(int status)
    {
        printf("SEAM: arch_shutdown(%d)\n", status);
        exit(43);
    }

    int arch_reboot(uint32_t) { return 0; }

    void kickos_terminal_outcome_apply(void)
    {
        printf("SEAM: kickos_terminal_outcome_apply\n");
    }

    void kfault_terminate(void)
    {
        printf("SEAM: kfault_terminate\n");
        exit(42);
    }

    // No stack switch, as on ARCH_SIM: a host thread stack is megabytes and no red-zone class
    // measures one.
    void kickos_panic_stack_enter(char const* msg, char const* file, unsigned line, uintptr_t)
    {
        kickos_panic_report(msg, file, line);
    }
}

namespace kickos
{
    bool dev_window_held_outside(uintptr_t, size_t, Task const*) { return false; }
}
