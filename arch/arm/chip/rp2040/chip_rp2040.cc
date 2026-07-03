// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Raspberry Pi RP2040 (Pico), Cortex-M0+ chip backend. Register addresses/fields
// are clean-room from the RP2040 datasheet (RP-008371-DS); hand-rolled, no vendor
// SDK sources, consistent with the arch layer's regs.h.
//
// M1 scope: privilege + SVC, no hardware MPU. Clocking is deliberately minimal --
// NO PLL: the 12 MHz crystal drives everything directly (clk_ref -> clk_sys and
// clk_peri = 12 MHz). That gives a precise UART baud and a precise SysTick without
// the PLL sequencing risk, which is the right posture for a first bring-up. The
// 64-bit monotonic clock (arch.h requires one; v6-M has no DWT) comes from the
// RP2040 system TIMER, fed by a 1 MHz tick divided from clk_ref.
//
// NOT run in this environment (no RP2040 model in mainline QEMU); verified by
// build + image inspection. Flash to a Pico (drag the UF2/.bin via BOOTSEL, or
// SWD) to confirm UART0 output on GP0 (pin 1). The board can always be recovered
// via BOOTSEL, so a wrong boot2/clock config cannot permanently brick it.
//
// The second-stage bootloader (boot2.S) and its CRC wrapper run BEFORE this file;
// by the time Reset_Handler executes, code is already executing in place from
// flash. See boot2.S and cmake/rp2040_checksum.py.

#include <kickos/arch/arch.h>

#include <stdint.h>

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    void kickos_armv6m_init(void);

    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    // clk_sys = clk_ref = XOSC = 12 MHz (no PLL). SysTick uses the processor
    // clock, so this is its tick rate for the ns->cycles conversion.
    uint32_t SystemCoreClock = 12000000u;
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }

    // Atomic register-access aliases (datasheet 2.1.2): every APB peripheral is
    // mirrored at base+0x2000 (bitmask SET on write) and base+0x3000 (bitmask
    // CLEAR on write), so a single-bit change needs no read-modify-write.
    constexpr uintptr_t ATOMIC_SET = 0x2000;
    constexpr uintptr_t ATOMIC_CLR = 0x3000;

    // --- RESETS (0x4000c000): peripherals are held in reset at power-up --------
    constexpr uintptr_t RESETS_BASE = 0x4000c000;
    constexpr uintptr_t RESETS_RESET = RESETS_BASE + 0x0;
    constexpr uintptr_t RESETS_RESET_DONE = RESETS_BASE + 0x8;
    constexpr uint32_t RESET_IO_BANK0 = 1u << 5;
    constexpr uint32_t RESET_PADS_BANK0 = 1u << 8;
    constexpr uint32_t RESET_TIMER = 1u << 21;
    constexpr uint32_t RESET_UART0 = 1u << 22;

    // --- XOSC (0x40024000): 12 MHz crystal ------------------------------------
    constexpr uintptr_t XOSC_BASE = 0x40024000;
    constexpr uintptr_t XOSC_CTRL = XOSC_BASE + 0x0;
    constexpr uintptr_t XOSC_STATUS = XOSC_BASE + 0x4;
    constexpr uintptr_t XOSC_STARTUP = XOSC_BASE + 0xc;
    constexpr uint32_t XOSC_FREQ_1_15MHZ = 0xaa0;    // CTRL.FREQ_RANGE
    constexpr uint32_t XOSC_ENABLE = 0xfabu << 12;   // CTRL.ENABLE magic
    constexpr uint32_t XOSC_STATUS_STABLE = 1u << 31;
    // STARTUP.DELAY counts in units of 256 crystal periods; ceil(12e6*1ms/256)=47.
    constexpr uint32_t XOSC_STARTUP_DELAY = 47;

    // --- CLOCKS (0x40008000) --------------------------------------------------
    constexpr uintptr_t CLOCKS_BASE = 0x40008000;
    constexpr uintptr_t CLK_REF_CTRL = CLOCKS_BASE + 0x30;
    constexpr uintptr_t CLK_REF_SELECTED = CLOCKS_BASE + 0x38;
    constexpr uintptr_t CLK_PERI_CTRL = CLOCKS_BASE + 0x48;
    constexpr uint32_t CLK_REF_SRC_XOSC = 0x2;           // CTRL.SRC glitchless
    constexpr uint32_t CLK_REF_SELECTED_XOSC = 1u << 2;  // one-hot readback
    // clk_peri: ENABLE(bit11) | AUXSRC. XOSC_CLKSRC=0x4 normally; CLK_SYS=0x0 in
    // the degraded (no-crystal) fallback.
    constexpr uint32_t CLK_PERI_ENABLE_XOSC = (1u << 11) | (0x4u << 5);
    constexpr uint32_t CLK_PERI_ENABLE_CLK_SYS = (1u << 11) | (0x0u << 5);

    // --- WATCHDOG tick (0x40058000): source of the 1 MHz TIMER tick -----------
    constexpr uintptr_t WATCHDOG_TICK = 0x40058000 + 0x2c;
    // tick = clk_ref / CYCLES; ENABLE = bit 9. clk_ref is 12 MHz (XOSC) normally,
    // ~6.5 MHz (ROSC) in the fallback -> pick CYCLES to land near 1 MHz either way.
    constexpr uint32_t WATCHDOG_TICK_CFG = (1u << 9) | 12u;
    constexpr uint32_t WATCHDOG_TICK_CFG_ROSC = (1u << 9) | 7u;
    // ROSC reset frequency is ~6.5 MHz (uncalibrated); used only for approximate
    // SysTick timing if the crystal never comes up.
    constexpr uint32_t ROSC_NOMINAL_HZ = 6500000u;

    // --- TIMER (0x40054000): 64-bit microsecond monotonic counter -------------
    // RAW halves (no latching) so the 64-bit read stays core-safe without an IRQ
    // guard (see arch_clock_now), unlike the latching TIMELR/TIMEHR pair.
    constexpr uintptr_t TIMER_BASE = 0x40054000;
    constexpr uintptr_t TIMER_TIMERAWH = TIMER_BASE + 0x24;
    constexpr uintptr_t TIMER_TIMERAWL = TIMER_BASE + 0x28;

    // --- IO_BANK0 (0x40014000): pin function select ---------------------------
    constexpr uintptr_t IO_BANK0_BASE = 0x40014000;
    constexpr uintptr_t IO_GPIO0_CTRL = IO_BANK0_BASE + 0x04; // GP0 = UART0 TX
    constexpr uintptr_t IO_GPIO1_CTRL = IO_BANK0_BASE + 0x0c; // GP1 = UART0 RX
    constexpr uint32_t IO_FUNCSEL_UART = 2;                   // F2 = UART0

    // --- PADS_BANK0 (0x4001c000) ----------------------------------------------
    constexpr uintptr_t PADS_BANK0_BASE = 0x4001c000;
    constexpr uintptr_t PADS_GPIO0 = PADS_BANK0_BASE + 0x04;
    constexpr uintptr_t PADS_GPIO1 = PADS_BANK0_BASE + 0x08;
    constexpr uint32_t PAD_OD = 1u << 7; // output disable
    constexpr uint32_t PAD_IE = 1u << 6; // input enable

    // --- UART0 (0x40034000): ARM PL011 ----------------------------------------
    constexpr uintptr_t UART0_BASE = 0x40034000;
    constexpr uintptr_t UART0_DR = UART0_BASE + 0x00;
    constexpr uintptr_t UART0_FR = UART0_BASE + 0x18;
    constexpr uintptr_t UART0_IBRD = UART0_BASE + 0x24;
    constexpr uintptr_t UART0_FBRD = UART0_BASE + 0x28;
    constexpr uintptr_t UART0_LCR_H = UART0_BASE + 0x2c;
    constexpr uintptr_t UART0_CR = UART0_BASE + 0x30;
    constexpr uint32_t FR_TXFF = 1u << 5; // TX FIFO full
    // baud = clk_peri / (16 x (IBRD + FBRD/64)); 12 MHz, 115200 -> IBRD 6, FBRD 33.
    constexpr uint32_t UART_IBRD_115200 = 6;
    constexpr uint32_t UART_FBRD_115200 = 33;
    constexpr uint32_t LCR_H_8N1_FIFO = (0x3u << 5) | (1u << 4); // WLEN=8, FEN
    constexpr uint32_t CR_ENABLE = (1u << 0) | (1u << 8) | (1u << 9); // UARTEN,TXE,RXE

    // Bounded so a dead/missing crystal or stuck peripheral degrades instead of
    // hanging the boot forever (a silent hang leaves no LED/UART sign of life).
    // The cap is far longer than any legitimate wait (XOSC startup is ~1 ms).
    constexpr uint32_t POLL_TIMEOUT = 1000000u;

    bool wait_mask(uintptr_t addr, uint32_t mask)
    {
        for (uint32_t i = 0; i < POLL_TIMEOUT; i++)
        {
            if ((r32(addr) & mask) == mask)
            {
                return true;
            }
        }
        return false;
    }

    void unreset(uint32_t mask)
    {
        r32(RESETS_RESET + ATOMIC_CLR) = mask;
        wait_mask(RESETS_RESET_DONE, mask); // bounded; best-effort
    }

    void clocks_init()
    {
        // Try the 12 MHz crystal, then run the reference clock (and thus clk_sys)
        // and the peripheral clock from it -- one precise source. If the crystal
        // never stabilizes, degrade to the ROSC that clk_sys already runs on at
        // reset so the board still boots (approximate timing) instead of hanging.
        r32(XOSC_STARTUP) = XOSC_STARTUP_DELAY;
        // Program the frequency range, THEN start the oscillator (datasheet
        // sequence): a combined write is avoided so ENABLE never latches before
        // FREQ_RANGE is in place.
        r32(XOSC_CTRL) = XOSC_FREQ_1_15MHZ;
        r32(XOSC_CTRL + ATOMIC_SET) = XOSC_ENABLE;

        bool xosc_ok = wait_mask(XOSC_STATUS, XOSC_STATUS_STABLE);
        if (xosc_ok)
        {
            // clk_ref <- XOSC (glitchless mux); clk_sys follows to 12 MHz. Poll
            // the one-hot SELECTED before proceeding.
            r32(CLK_REF_CTRL) = CLK_REF_SRC_XOSC;
            xosc_ok = wait_mask(CLK_REF_SELECTED, CLK_REF_SELECTED_XOSC);
        }

        if (xosc_ok)
        {
            r32(CLK_PERI_CTRL) = CLK_PERI_ENABLE_XOSC; // UART clock <- XOSC 12 MHz
            r32(WATCHDOG_TICK) = WATCHDOG_TICK_CFG;    // 12 MHz / 12 = 1 MHz tick
        }
        else
        {
            SystemCoreClock = ROSC_NOMINAL_HZ;             // clk_sys stayed on ROSC
            r32(CLK_PERI_CTRL) = CLK_PERI_ENABLE_CLK_SYS;  // UART clock <- clk_sys
            r32(WATCHDOG_TICK) = WATCHDOG_TICK_CFG_ROSC;   // ~6.5 MHz / 7 ~= 1 MHz
        }
    }

    void uart0_init()
    {
        // Route GP0/GP1 to UART0 and make the pads usable (TX drives out, RX in).
        r32(IO_GPIO0_CTRL) = IO_FUNCSEL_UART;
        r32(IO_GPIO1_CTRL) = IO_FUNCSEL_UART;
        r32(PADS_GPIO0 + ATOMIC_CLR) = PAD_OD;
        r32(PADS_GPIO1 + ATOMIC_SET) = PAD_IE;

        // Divisors latch only on the subsequent LCR_H write, so order matters.
        r32(UART0_IBRD) = UART_IBRD_115200;
        r32(UART0_FBRD) = UART_FBRD_115200;
        r32(UART0_LCR_H) = LCR_H_8N1_FIFO;
        r32(UART0_CR) = CR_ENABLE;
    }
}

extern "C"
{

void arch_init(void)
{
    // Reset-release ordering is load-bearing: a peripheral's RESET_DONE only
    // asserts once it has a running clock. IO_BANK0/PADS_BANK0/TIMER are clocked
    // by clk_sys/clk_ref (already live off the ROSC at reset), so release them
    // now. UART0 is clocked by clk_peri, which is OFF until clocks_init -- release
    // it BEFORE that and its RESET_DONE never asserts, hanging the boot.
    unreset(RESET_IO_BANK0 | RESET_PADS_BANK0 | RESET_TIMER);
    clocks_init();
    unreset(RESET_UART0);
    uart0_init();
    kickos_armv6m_init();
}

void arch_console_write(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        while ((r32(UART0_FR) & FR_TXFF) != 0)
        {
        }
        r32(UART0_DR) = static_cast<uint8_t>(buf[i]);
    }
}

// Monotonic clock from the 64-bit system TIMER (microseconds -> ns). Uses the
// non-latching RAW halves with a hi/lo/hi re-read to tolerate a 32-bit rollover
// between the reads. This needs no interrupt guard and stays correct if a future
// milestone launches core 1 (the latching TIMELR/TIMEHR pair is single-core only).
uint64_t arch_clock_now(void)
{
    uint32_t hi = r32(TIMER_TIMERAWH);
    uint32_t lo;
    while (true)
    {
        lo = r32(TIMER_TIMERAWL);
        uint32_t hi2 = r32(TIMER_TIMERAWH);
        if (hi2 == hi)
        {
            break;
        }
        hi = hi2;
    }
    return ((static_cast<uint64_t>(hi) << 32) | lo) * 1000ull;
}

void arch_shutdown(int status)
{
    (void)status; // no exit on bare metal
    __asm volatile("cpsid i" ::: "memory");
    while (true)
    {
        __asm volatile("wfi");
    }
}

void Reset_Handler(void)
{
    // Cortex-M0+ has no FPU; nothing to enable before the C runtime.
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

void HardFault_Handler(void)
{
    arch_shutdown(132);
}

}
