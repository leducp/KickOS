// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The 12 MHz crystal oscillator both parts carry (RP2040 DS 2.16 / RP2350 DS 8.2), included
// inside a chip's reg::xosc namespace. Program FREQ_RANGE before writing ENABLE, then poll
// STATUS.STABLE.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2XXX_REGS_XOSC_INC
#define KICKOS_ARCH_ARM_CHIP_RP2XXX_REGS_XOSC_INC

constexpr uintptr_t CTRL = mmap::XOSC_BASE + 0x0u;
constexpr uintptr_t STATUS = mmap::XOSC_BASE + 0x4u;
constexpr uintptr_t STARTUP = mmap::XOSC_BASE + 0xcu;

constexpr uint32_t FREQ_HZ = 12000000u;     // the crystal both parts are specified for
constexpr uint32_t FREQ_1_15MHZ = 0xaa0u;   // CTRL.FREQ_RANGE (1..15 MHz)
constexpr uint32_t ENABLE = 0xfabu << 12;   // CTRL.ENABLE magic
constexpr uint32_t STATUS_STABLE = 1u << 31;
// STARTUP.DELAY counts in units of 256 crystal periods; ceil(12e6*1ms/256)=47.
constexpr uint32_t STARTUP_DELAY = 47u;

#endif
