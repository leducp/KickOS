// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 LP node. It has RV32IMAC in M mode only and runs its image from
// node 1's HP-SRAM slice. The HP node loads the LP SRAM reset vector and wakes
// this core through PMU; the common RV32 switcher handles its one vector.

#include <kickos/arch/arch.h>

#if KICKOS_RV32_LP

#include <kickos/sys/atomic.h>

#include <stdint.h>

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    void kickos_rv32_init(void);
    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();
    extern uint32_t __eh_frame_start;
    void __register_frame(void*) __attribute__((weak));
    uint32_t SystemCoreClock = 20000000u;
    extern kickos::Atomic<uint32_t, kickos::Order::RELAXED> kickos_c6_amp_rtc_hz;
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    constexpr uintptr_t RTC = 0x600B0C00u;
    uint64_t rtc_ticks()
    {
        r32(RTC + 0x10u) |= 1u << 28; // latch the current 48-bit slow-clock count
        uint32_t const lo = r32(RTC + 0x14u);
        uint32_t const hi = r32(RTC + 0x18u) & 0xFFFFu;
        return (static_cast<uint64_t>(hi) << 32) | lo;
    }
}

extern "C"
{
int arch_rv_has_mcounteren(void) { return 0; }

void arch_amp_release_peers(void) {}

void arch_init(void)
{
    r32(RTC + 0x0Cu) = 0;
    r32(RTC + 0x44u) = 1u << 31;
    r32(RTC + 0x40u) |= 1u << 31;
    kickos_rv32_init();
}

uint64_t arch_clock_now(void)
{
    uint32_t const hz = kickos_c6_amp_rtc_hz;
    uint64_t const ticks = rtc_ticks();
    return (ticks / hz) * 1000000000ull + ((ticks % hz) * 1000000000ull) / hz;
}

void arch_timer_arm(uint64_t deadline_ns)
{
    uint32_t const hz = kickos_c6_amp_rtc_hz;
    // Splitting at a second keeps the product in range for the RTC's whole
    // 48-bit lifetime. The divisor is constant, so no variable 64-bit divide
    // sits in the masked timer-arm path.
    uint64_t const ticks = (deadline_ns / 1000000000ull) * hz
                           + ((deadline_ns % 1000000000ull) * hz) / 1000000000ull;
    r32(RTC + 0x0Cu) = 0;
    r32(RTC + 0x08u) = static_cast<uint32_t>(ticks);
    r32(RTC + 0x0Cu) = (1u << 31) | static_cast<uint32_t>((ticks >> 32) & 0xFFFFu);
}

void arch_timer_disarm(void)
{
    r32(RTC + 0x0Cu) = 0;
    r32(RTC + 0x44u) = 1u << 31;
}

int arch_console_write(char const*, size_t n)
{
    // This port wires no LP UART. The LP's boot and served-call witnesses travel
    // through the shared AMP diagnostic and app records, which HP reports.
    return static_cast<int>(n);
}

int arch_console_write_retry(char const* buf, size_t n, bool* cr_pending)
{
    if (cr_pending != nullptr)
    {
        *cr_pending = false;
    }
    return arch_console_write(buf, n);
}

bool arch_console_write_sync(char const*, size_t)
{
    return true;
}

void arch_shutdown(int)
{
    __asm volatile("csrci mstatus, 0x8" ::: "memory");
    while (true)
    {
        __asm volatile("wfi");
    }
}

void Reset_Handler(void)
{
    for (uint32_t* p = &_sdata, *s = &_sidata; p < &_edata;)
    {
        *p++ = *s++;
    }
    for (uint32_t* p = &_sbss; p < &_ebss; p++)
    {
        *p = 0;
    }
    if (__register_frame != nullptr)
    {
        __register_frame(&__eh_frame_start);
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

#endif
