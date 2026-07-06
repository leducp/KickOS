// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-D0WDQ6 (WROOM-32) chip backend. Register addresses are clean-room facts
// transcribed from the ESP32 TRM (register base addresses per TRM 1.5.3 "System
// and Memory / peripheral base"; UART/WDT register offsets per the UART and
// Watchdog chapters). Hand-rolled, no ESP-IDF/HAL sources.
//
// M1.x scope (build-only, HW deferred): the CPU runs at whatever clock the ROM
// left -- no PLL bring-up yet -- exactly the K64F posture ("runs at the reset
// clock"). The ROM already initialized UART0 (GPIO1 TX / GPIO3 RX) at 115200, so
// arch_console_write just polls its TX FIFO. NOT run on hardware; the port is
// verified by build + image inspection only.

#include <kickos/arch/arch.h>

#include <stdint.h>

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    void kickos_lx6_init(void);

    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    // The ROM first-stage loader leaves the CPU on the 40 MHz crystal (no PLL,
    // since KickOS boots without the IDF second-stage bootloader). Recorded here
    // for the CCOUNT-based clock; a later refinement brings up the PLL to 240 MHz.
    uint32_t SystemCoreClock = 40000000u;
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

    // --- UART0 (TRM UART chapter). ROM leaves it configured at 115200 8N1. ---
    constexpr uintptr_t UART0_BASE = 0x3FF40000;
    constexpr uintptr_t UART0_FIFO = UART0_BASE + 0x00;
    constexpr uintptr_t UART0_STATUS = UART0_BASE + 0x1C;
    // STATUS.TXFIFO_CNT is bits [23:16]: bytes currently in the 128-entry TX FIFO.
    constexpr uint32_t TXFIFO_CNT_SHIFT = 16;
    constexpr uint32_t TXFIFO_CNT_MASK = 0xFF;
    constexpr uint32_t TXFIFO_LIMIT = 126; // leave headroom below the 128-deep FIFO

    // --- Watchdogs. The ROM (running the image in flash-boot mode) leaves THREE
    //     watchdogs armed: the RTC WDT and the two Timer Group MWDTs (TIMG0, TIMG1).
    //     Each must be fully disabled or it resets the part within seconds of
    //     bring-up. Each register file is unlocked by writing its 32-bit write-
    //     protect key (default 0x50D83AA1), edited, then re-locked (write 0).
    //
    //     Clearing WDT_EN alone is NOT enough: the ROM arms the stage-0 watchdog
    //     via the separate FLASHBOOT_MOD_EN bit (a flash-boot watchdog independent
    //     of WDT_EN), which stays live until explicitly cleared. So each WDT needs
    //     both WDT_EN and FLASHBOOT_MOD_EN cleared. Bits/offsets are from the ESP32
    //     TRM (RTC_CNTL + TIMG watchdog register chapters). NOTE: the classic ESP32
    //     has NO RTC super-watchdog (SWD) --
    //     that block (RTC_CNTL_SWD_*) was introduced on the ESP32-S2 and later, so
    //     there is nothing more to disable here. ---
    constexpr uint32_t WDT_WKEY = 0x50D83AA1;         // RTC + TIMG MWDT write-protect key
    constexpr uint32_t WDT_EN = 1u << 31;             // {RTC,TIMG}_WDTCONFIG0.WDT_EN
    constexpr uint32_t RTC_WDT_FLASHBOOT_EN = 1u << 10;  // RTC_CNTL_WDT_FLASHBOOT_MOD_EN
    constexpr uint32_t TIMG_WDT_FLASHBOOT_EN = 1u << 14; // TIMG_WDT_FLASHBOOT_MOD_EN

    constexpr uintptr_t RTC_CNTL_BASE = 0x3FF48000;
    constexpr uintptr_t RTC_WDTCONFIG0 = RTC_CNTL_BASE + 0x8C;
    constexpr uintptr_t RTC_WDTWPROTECT = RTC_CNTL_BASE + 0xA4;

    // Timer groups are 0x1000 apart (TIMG0 @ 0x3FF5F000, TIMG1 @ 0x3FF60000).
    constexpr uintptr_t TIMG0_BASE = 0x3FF5F000;
    constexpr uintptr_t TIMG1_BASE = 0x3FF60000;
    constexpr uintptr_t TIMG_WDTCONFIG0_OFF = 0x48;
    constexpr uintptr_t TIMG_WDTWPROTECT_OFF = 0x64;

    void timg_wdt_disable(uintptr_t base)
    {
        r32(base + TIMG_WDTWPROTECT_OFF) = WDT_WKEY;
        r32(base + TIMG_WDTCONFIG0_OFF) &= ~(WDT_EN | TIMG_WDT_FLASHBOOT_EN);
        r32(base + TIMG_WDTWPROTECT_OFF) = 0;
    }

    void wdt_disable()
    {
        r32(RTC_WDTWPROTECT) = WDT_WKEY;
        r32(RTC_WDTCONFIG0) &= ~(WDT_EN | RTC_WDT_FLASHBOOT_EN);
        r32(RTC_WDTWPROTECT) = 0;

        timg_wdt_disable(TIMG0_BASE);
        timg_wdt_disable(TIMG1_BASE);
    }
}

extern "C"
{

void arch_init(void)
{
    // FP: the LX6 single-precision FPU (coprocessor 0) IS enabled for all threads
    // (kickos_lx6_init sets CPENABLE), so `float` works here exactly as it does on
    // the Cortex-M4F boards and via soft-float on the M0/M3 boards -- a thread that
    // compiles clean on one KickOS board must not fault on another. The FP data
    // registers are caller-saved on Xtensa (the compiler spills live f-regs around
    // any call), so the COOPERATIVE switch needs no FP handling; only the PREEMPTIVE
    // path banks them -- the level-1 interrupt frame saves/restores f0-f15+FCR+FSR
    // (startup.S). Double stays soft-float (__muldf3): the LX6 FPU is single-only.
    wdt_disable();
    kickos_lx6_init();
}

void arch_console_write(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        while (((r32(UART0_STATUS) >> TXFIFO_CNT_SHIFT) & TXFIFO_CNT_MASK) >= TXFIFO_LIMIT)
        {
        }
        r32(UART0_FIFO) = static_cast<uint8_t>(buf[i]);
    }
}

void arch_shutdown(int status)
{
    (void)status; // no exit on bare metal
    // RSIL 15 then WAITI 15: mask everything (incl. below NMI) and park -- the
    // RP2040/K64F "cpsid i; wfi" twin. WAITI writes PS.INTLEVEL from its immediate,
    // so it must be 15, not 0 (waiti 0 would unmask everything the rsil masked).
    __asm volatile("rsil a0, 15" ::: "a0", "memory");
    while (true)
    {
        __asm volatile("waiti 15");
    }
}

void Reset_Handler(void)
{
    // .data is already in RAM (the image links data at its VMA), but keep the copy
    // loop for uniformity with the ARM ports -- it is a no-op when LMA == VMA.
    uint32_t* src = &_sidata;
    uint32_t* dst = &_sdata;
    while (dst < &_edata)
    {
        *dst++ = *src++;
    }
    for (uint32_t* b = &_sbss; b < &_ebss; b++)
    {
        *b = 0;
    }
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}

}
