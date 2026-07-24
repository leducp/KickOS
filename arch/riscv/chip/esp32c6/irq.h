// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 interrupt-source numbers.
//
// Two distinct numbering spaces meet on this core:
//  1. CPU interrupt IDs (0..31). The C6 vectors mcause = ID (Espressif's custom
//     scheme, not the standard mcause=11). The local CLINT owns 3 (msip) and 7
//     (mtip); external device IDs come from 1-2, 5-6, 8-31. The two IDs KickOS
//     claims -- the software-inject doorbell (31) and the real-device line (30) --
//     are owned by the shared arch header <kickos/arch/rv_trap_ids.h>
//     (KICKOS_RV_INJECT_DOORBELL_CPU_INT / KICKOS_RV_DEV_CPU_INT) so switch.S and
//     the chip layer cannot drift; they are NOT redefined here. Mirrored below as
//     enumerators for reference only.
//  2. Logical kernel IRQ lines (kickos_isr_irq / irq_register). Chip-local.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_IRQ_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_IRQ_H

#include <stdint.h>

namespace kickos::esp32c6::irq
{
    // Logical kernel IRQ line: UART0 TX-empty -> buffered console ring drain.
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
