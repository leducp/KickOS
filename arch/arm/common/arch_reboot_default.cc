// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a chip
// definition keeps this archive member unextracted. Every ARMv6-M/v7-M/v8-M part resets
// through SCB AIRCR.SYSRESETREQ; a bootloader entry is a chip's own.

#include <kickos/arch/arch.h>
#include <arch_reboot_tail.h>
#include <kickos/diag.h>
#include <kickos/sys/abi.h>

#include "regs.h"

extern "C" int arch_reboot(uint32_t outcome)
{
    if (outcome != KOS_SHUTDOWN_REBOOT)
    {
        return -KOS_ENOSYS;
    }
    kickos::arm::system_reset_request();
    kickos::arch::reset_settle();
    kickos::arch::reboot_failed(kickos::diag::kResetNotTaken);
}
