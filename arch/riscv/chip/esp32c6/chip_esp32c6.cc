// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6-WROOM-1 (ESP-RISC-V "HP CPU", RV32IMAC) chip backend. Shares the
// rv32imac arch with the qemu-virt board; this layer supplies the hardware edges:
// reset/C-runtime bring-up, watchdog disable, the console, arch_shutdown, and the
// tickless clock/one-shot timer.
//
// The C6 has a memory-mapped core-local CLINT (TRM ch.1.7) that provides the exact
// same seam as the qemu-virt SiFive CLINT: MSIP (machine software interrupt =
// deferred switch, mcause 3) + MTIME/MTIMECMP (machine timer = tickless tick,
// mcause 7). So the scheduler mechanism is identical to virt; only the base
// address + the MTCE counter-enable differ. Console is the native USB Serial/JTAG
// (the same USB the board is flashed/debugged over -- Waveshare C6-DEV-KIT).
//
// Register addresses: ESP32-C6 TRM v1.2 (memory map Table 5.3-2; CLINT ch.1.7;
// watchdogs ch.14/15; USB Serial/JTAG ch.32). Hand-rolled, no ESP-IDF/HAL sources.
// STATUS: register-accurate but NOT yet run on silicon -- bring-up validates on HW
// (esptool flash over USB + OpenOCD USB-JTAG). Items marked TODO(HW) need a scope/
// silicon check: the MTIME tick rate (affects only timing accuracy, not boot) and a
// few bit positions the TRM's ASCII diagrams left ambiguous.

#include <kickos/arch/arch.h>

#include <stdint.h>

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    void kickos_rv32_init(void);
    extern volatile uint32_t* g_clint_msip;

    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    // Core clock in Hz. The ROM first-stage loader leaves the CPU on XTAL/1 = 40 MHz
    // (TRM §8.2); no PLL bring-up here (the classic-ESP32 posture). Feeds the bench
    // rdcycle->ns print; a later refinement brings up the 160 MHz PLL.
    uint32_t SystemCoreClock = 40000000u;
}

namespace
{
    inline volatile uint32_t* r32p(uintptr_t a) { return reinterpret_cast<volatile uint32_t*>(a); }
    inline volatile uint32_t& r32(uintptr_t a) { return *r32p(a); }

    // --- Core-local CLINT (TRM ch.1.7; CPU sub-system base 0x2000_0000, regs @0x1800).
    constexpr uintptr_t CLINT_MSIP = 0x20001800;     // bit0: machine software int pending (switch)
    constexpr uintptr_t CLINT_MTIMECTL = 0x20001804; // MTCE|MTIE|MTIP|MTOF
    constexpr uintptr_t CLINT_MTIME = 0x20001808;    // 64-bit counter
    constexpr uintptr_t CLINT_MTIMECMP = 0x20001810; // 64-bit compare
    constexpr uint32_t MTIMECTL_MTCE = 1u << 0;      // enable the timer counter
    constexpr uint32_t MTIMECTL_MTIE = 1u << 1;      // enable the timer interrupt
    // TODO(HW): confirm the MTIME tick rate on silicon (scope/known-delay). Only
    // affects sleep/timeout accuracy, not boot or the switch. 16 MHz is a plausible
    // placeholder (matches the SYSTIMER CNT_CLK); tune once measured.
    constexpr uint64_t MTIME_HZ = 16000000ull;
    constexpr uint64_t NS_PER_TICK = 1000000000ull / MTIME_HZ;

    // --- USB Serial/JTAG console (TRM ch.32; base 0x6000_F000). The ROM leaves it
    //     enumerated (it is the boot/console/flash path on this board), so we just
    //     write bytes: poll EP1_CONF[0] for FIFO room, write to EP1, flush via [1].
    constexpr uintptr_t USB_EP1 = 0x6000F000;             // RDWR_BYTE [7:0]
    constexpr uintptr_t USB_EP1_CONF = 0x6000F004;
    constexpr uint32_t USB_IN_EP_DATA_FREE = 1u << 0;     // RO: FIFO has room
    constexpr uint32_t USB_WR_DONE = 1u << 1;             // WT: flush buffered bytes to host

    // --- Watchdogs (TRM ch.14 MWDT, ch.15 RWDT/SWD). ALL must be disabled or the
    //     ROM-armed WDTs reset the part within seconds. Common unlock key 0x50D83AA1.
    constexpr uint32_t WDT_WKEY = 0x50D83AA1;
    constexpr uintptr_t TIMG0_BASE = 0x60008000;
    constexpr uintptr_t TIMG1_BASE = 0x60009000;
    constexpr uintptr_t TIMG_WDTCONFIG0 = 0x0048;         // EN=bit31, FLASHBOOT_MOD_EN=bit14
    constexpr uintptr_t TIMG_WDTWPROTECT = 0x0064;
    constexpr uint32_t TIMG_WDT_EN = 1u << 31;
    constexpr uint32_t TIMG_WDT_FLASHBOOT = 1u << 14;

    constexpr uintptr_t RTC_WDT_BASE = 0x600B1C00;
    constexpr uintptr_t RTC_WDT_CONFIG0 = 0x0000;         // EN=bit31, FLASHBOOT_MOD_EN=bit12
    constexpr uintptr_t RTC_WDT_WPROTECT = 0x0018;
    constexpr uint32_t RTC_WDT_EN = 1u << 31;
    constexpr uint32_t RTC_WDT_FLASHBOOT = 1u << 12;
    constexpr uintptr_t RTC_SWD_CONFIG = 0x001C;          // SWD_DISABLE=bit30
    constexpr uintptr_t RTC_SWD_WPROTECT = 0x0020;
    constexpr uint32_t RTC_SWD_DISABLE = 1u << 30;

    void timg_mwdt_disable(uintptr_t base)
    {
        r32(base + TIMG_WDTWPROTECT) = WDT_WKEY;                       // unlock
        r32(base + TIMG_WDTCONFIG0) &= ~(TIMG_WDT_EN | TIMG_WDT_FLASHBOOT);
        r32(base + TIMG_WDTWPROTECT) = 0;                             // re-lock
    }

    void wdt_disable_all()
    {
        timg_mwdt_disable(TIMG0_BASE);
        timg_mwdt_disable(TIMG1_BASE);
        // RTC (LP) watchdog.
        r32(RTC_WDT_BASE + RTC_WDT_WPROTECT) = WDT_WKEY;
        r32(RTC_WDT_BASE + RTC_WDT_CONFIG0) &= ~(RTC_WDT_EN | RTC_WDT_FLASHBOOT);
        r32(RTC_WDT_BASE + RTC_WDT_WPROTECT) = 0;
        // Super watchdog (SWD): set the disable bit (its own write-protect key).
        r32(RTC_WDT_BASE + RTC_SWD_WPROTECT) = WDT_WKEY;
        r32(RTC_WDT_BASE + RTC_SWD_CONFIG) |= RTC_SWD_DISABLE;
        r32(RTC_WDT_BASE + RTC_SWD_WPROTECT) = 0;
    }
}

extern "C"
{

// --- Console: native USB Serial/JTAG. Bounded poll so a disconnected/undrained
//     host drops bytes instead of hanging the kernel.
void arch_console_write(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint32_t spin = 0;
        while ((r32(USB_EP1_CONF) & USB_IN_EP_DATA_FREE) == 0)
        {
            if (++spin > 200000u)
            {
                return; // host not draining the FIFO -> drop (never block the kernel)
            }
        }
        r32(USB_EP1) = static_cast<uint8_t>(buf[i]);
    }
    r32(USB_EP1_CONF) = USB_WR_DONE; // push the buffered bytes to the host
}

// --- Tickless clock: the 64-bit CLINT MTIME -> ns -------------------------------
uint64_t arch_clock_now(void)
{
    volatile uint32_t* mt = r32p(CLINT_MTIME);
    uint32_t hi, lo, hi2;
    do
    {
        hi = mt[1];
        lo = mt[0];
        hi2 = mt[1];
    } while (hi != hi2);
    uint64_t t = (static_cast<uint64_t>(hi) << 32) | lo;
    return t * NS_PER_TICK;
}

// --- One-shot next-event timer: CLINT MTIMECMP (fires when MTIME >= MTIMECMP) ----
void arch_timer_arm(uint64_t deadline_ns)
{
    uint64_t ticks = deadline_ns / NS_PER_TICK;
    volatile uint32_t* cmp = r32p(CLINT_MTIMECMP);
    cmp[1] = 0xFFFFFFFFu; // park high half so no spurious match between the two stores
    cmp[0] = static_cast<uint32_t>(ticks);
    cmp[1] = static_cast<uint32_t>(ticks >> 32);
}

void arch_timer_disarm(void)
{
    volatile uint32_t* cmp = r32p(CLINT_MTIMECMP);
    cmp[0] = 0xFFFFFFFFu;
    cmp[1] = 0xFFFFFFFFu;
}

void arch_init(void)
{
    wdt_disable_all(); // or the ROM-armed watchdogs reset the part in seconds

    g_clint_msip = r32p(CLINT_MSIP);   // the deferred-switch software interrupt
    arch_timer_disarm();               // MTIMECMP = max: no timer fire until armed
    r32(CLINT_MTIMECTL) = MTIMECTL_MTCE | MTIMECTL_MTIE; // start the counter + enable

    kickos_rv32_init(); // vectored mtvec + mie(MSIE|MTIE|SSIE) + mcounteren + PMP
    // Console/inject via arch defaults; the interrupt matrix (device IRQs) is not
    // wired -- hello needs only the CLINT (switch + tick) + ecall, exactly like virt.
}

void arch_shutdown(int status)
{
    (void)status; // no exit on bare metal
    __asm volatile("csrci mstatus, 0x8" ::: "memory"); // mask interrupts (clear MIE)
    while (true)
    {
        __asm volatile("wfi");
    }
}

// --- C-runtime bring-up (the reset entry, called by _start in startup.S) ------
void Reset_Handler(void)
{
    // The ROM loader copies the image segments to SRAM (LMA == VMA), so the copy is
    // a no-op; kept for uniformity with the other ports.
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
