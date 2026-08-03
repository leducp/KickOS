// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 interrupt-source numbers.
//
// Two distinct numbering spaces meet on this core:
//  1. CPU interrupt IDs (0..31). The C6 vectors mcause = ID, not the standard
//     mcause = 11. The local CLINT owns 3 (msip) and 7 (mtip); external device IDs
//     come from 1-2, 5-6, 8-31. The two IDs KickOS claims (software-inject doorbell
//     31, real-device line 30) are owned by the shared arch header
//     <kickos/arch/rv_trap_ids.h> (KICKOS_RV_INJECT_DOORBELL_CPU_INT /
//     KICKOS_RV_DEV_CPU_INT) and are NOT redefined here; the enumerators below only
//     mirror them.
//  2. Logical kernel IRQ lines (kickos_isr_irq / irq_register). Chip-local.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_IRQ_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_IRQ_H

#include <stdint.h>

namespace kickos::esp32c6::irq
{
    // Logical kernel IRQ lines. UART0_TX_LINE is the GROUPED UART0 line: every UART0
    // sub-source (TX-empty, RX-full, RX-timeout, overrun, framing, parity) shares one
    // interrupt-matrix source (no.43, TRM section 10.3.1) and one UART_INT_ST word, and the
    // only kernel-owned mask on this chip gates the whole CPU interrupt, so the
    // sub-sources cannot be separately maskable lines. The chip demuxes them
    // (kickos_rv_ext_dispatch_dev) and they all post here.
    enum kernel_line : int
    {
        UART0_TX_LINE = 16,
    };

    // CPU interrupt IDs. Reference mirror of the arch-owned rv_trap_ids.h macros
    // (do not treat these as the source of truth; that header is).
    enum cpu_int : uint32_t
    {
        CPU_INT_MSIP = 3,          // CLINT machine software int (deferred switch)
        CPU_INT_MTIP = 7,          // CLINT machine timer (tickless tick)
        CPU_INT_DEVICE = 30,       // == KICKOS_RV_DEV_CPU_INT (UART0 TX line)
        CPU_INT_INJECT_DOORBELL = 31, // == KICKOS_RV_INJECT_DOORBELL_CPU_INT
    };
}

#endif
