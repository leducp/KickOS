// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What kernel/init/terminal.cc reaches: the authority check, the mask, and the chip's reset,
// which records the outcome it was asked for.

#include <kickos/arch/arch.h>
#include <kickos/cap.h>

#include <kickos/sys/errno.h>

#include "reset_seam.h"

namespace kickos
{
    namespace resetfix
    {
        uint32_t g_authority = 0;
        uint32_t g_reboot_outcome = REBOOT_NONE;
    }

    bool cap_check_authority(Thread*, uint32_t need)
    {
        return (resetfix::g_authority & need) == need;
    }
}

extern "C"
{
    arch_irq_state_t arch_irq_save(void)
    {
        return 0;
    }

    void arch_irq_restore(arch_irq_state_t)
    {
    }

    int arch_reboot(uint32_t outcome)
    {
        kickos::resetfix::g_reboot_outcome = outcome;
        return -KOS_ENOSYS;
    }
}
