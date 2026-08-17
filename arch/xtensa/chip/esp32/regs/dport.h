// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-D0WDQ6 DPORT register map (ESP32 TRM DPORT / interrupt-matrix chapters):
// CPU clock-period select and the peripheral-source-to-CPU-interrupt matrix.

#ifndef KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_DPORT_H
#define KICKOS_ARCH_XTENSA_CHIP_ESP32_REGS_DPORT_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::esp32::reg::dport
{
    constexpr uintptr_t CPU_PER_CONF = mmap::DPORT_BASE + 0x3Cu;

    // Interrupt-matrix map register for the UART0 source (peripheral source 34).
    // Written with the TARGET CPU interrupt number (see irq::cpu_int::UART0_CPU_INT).
    constexpr uintptr_t PRO_UART_INTR_MAP = mmap::DPORT_BASE + 0x18Cu;

    // CPU_PER_CONF.CPUPERIOD_SEL [1:0]: 0=80 1=160 2=240 MHz.
    constexpr uint32_t CPUPERIOD_SEL_240 = 2;
}

#endif
