// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The semihosting console and exit of the chips QEMU runs with -semihosting. A chip
// takes the console seams by compiling semihost_console.cc into its own archive (`family`).

#ifndef KICKOS_ARCH_COMMON_SEMIHOST_H
#define KICKOS_ARCH_COMMON_SEMIHOST_H

#include <kickos/arch/arch.h>

#include <stdint.h>

#include "semihost_trap.h"

namespace kickos::semihost
{
    constexpr long SYS_WRITEC = 0x03;
    constexpr long SYS_EXIT_EXTENDED = 0x20;
    constexpr uint32_t ADP_Stopped_ApplicationExit = 0x20026u;

    inline __attribute__((always_inline, noreturn)) void exit(int status)
    {
        uint32_t block[2];
        block[0] = ADP_Stopped_ApplicationExit;
        block[1] = static_cast<uint32_t>(status);
        call(SYS_EXIT_EXTENDED, block);
        halt_masked();
    }
}

#endif
