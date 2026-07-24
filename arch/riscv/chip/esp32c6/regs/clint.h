// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 core-local CLINT registers (TRM v1.2 ch.1.7). Same seam as the qemu-virt
// SiFive CLINT: MSIP (machine software int = deferred switch, mcause 3) + MTIME/
// MTIMECMP (machine timer = tickless tick, mcause 7). The C6 adds MTIMECTL with an
// explicit counter enable (MTCE) that virt does not have.
//
// MTIME is core-clocked: MEASURED ~160 MHz on silicon (2026-07-09), NOT the 16 MHz
// SYSTIMER rate first assumed. 1e9/160e6 = 6.25 ns/tick = 25/4 exactly. If a future
// clock bring-up changes the CPU frequency, MTIME_HZ must track it.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_CLINT_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_CLINT_H

#include "../mmap.h"

#include <stdint.h>

namespace kickos::esp32c6::reg::clint
{
    constexpr uintptr_t MSIP = mmap::CLINT_BASE + 0x00u;     // bit0: machine software int pending
    constexpr uintptr_t MTIMECTL = mmap::CLINT_BASE + 0x04u; // MTCE|MTIE|MTIP|MTOF
    constexpr uintptr_t MTIME = mmap::CLINT_BASE + 0x08u;    // 64-bit counter (lo @+0x08, hi @+0x0C)
    constexpr uintptr_t MTIMECMP = mmap::CLINT_BASE + 0x10u; // 64-bit compare (lo @+0x10, hi @+0x14)

    constexpr uint32_t MTIMECTL_MTCE = 1u << 0; // enable the timer counter
    constexpr uint32_t MTIMECTL_MTIE = 1u << 1; // enable the timer interrupt

    constexpr uint64_t MTIME_HZ = 160000000ull; // MEASURED on silicon (see file header)
}

#endif
