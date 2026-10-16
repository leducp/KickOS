// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What syscall_witness.cc reaches beyond the region seam: the console it announces on and the
// timer and interrupted-flag seams of its interrupt arm, which no case here takes.

#include <kickos/arch/arch.h>

#include <stdarg.h>
#include <stdint.h>

namespace kickos
{
    void kprintf(char const* fmt, ...)
    {
        (void)fmt;
    }
}

extern "C"
{
    void arch_timer_arm(uint64_t deadline_ns)
    {
        (void)deadline_ns;
    }

    int arch_witness_irq_lift_flag(void)
    {
        return 0;
    }
}
