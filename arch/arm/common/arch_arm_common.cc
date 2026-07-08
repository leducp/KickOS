// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Core-generic ARM Cortex-M arch backend: the parts of the arch.h seam whose
// implementation is IDENTICAL on ARMv6-M and ARMv7-M (deferred PendSV switch,
// SysTick one-shot timer, NVIC mask/unmask/inject, the RAM bump allocator, the
// idle wait, and the SysTick ISR entry). Compiled into BOTH kickos_arch_armv6m
// and kickos_arch_armv7m.
//
// The arch-profile-specific edges stay in arch_armv{6,7}m.cc: context init (the
// v7-M frame carries an EXC_RETURN word), the critical section (PRIMASK vs
// BASEPRI), NVIC priority programming, the monotonic/trace clock source, and
// per-arch bring-up + the distinctly-named default-IRQ entry.

#include <kickos/arch/arch.h>

#include <kickos/units.h> // _s literal (== 1e9 ns) for the ns/cycle conversions

#include "regs.h"

#include <stddef.h>

namespace
{
    using namespace kickos::arm;
    using namespace kickos::units; // _s == 1e9 ns

    // User-RAM region for arch_ram_alloc, defined by the chip linker script.
    // Bump-allocated; freed only wholesale (matches the sim arena's M0 model).
    volatile uint32_t g_ram_used = 0;
}

extern "C"
{
    // Linker-provided user-RAM bounds (chip linker script, M1 item 10).
    extern unsigned char __kickos_ram_start[];
    extern unsigned char __kickos_ram_end[];

    // CMSIS convention: the core clock in Hz, defined + maintained by the chip.
    extern uint32_t SystemCoreClock;

    // Shared with switch.S (the PendSV/arch_start switch targets). volatile:
    // written by C (arch_switch) and by asm (PendSV); the compiler must not
    // cache or elide the accesses it can't see across the asm seam.
    struct arch_context* volatile g_arch_current = nullptr;
    struct arch_context* volatile g_arch_next = nullptr;
}

// ===========================================================================
extern "C"
{

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
void arch_trace_stamp_id(struct arch_context* ctx, uint16_t id)
{
    ctx->trace_tid = id;
}
#endif

// --- Switch: always deferred to PendSV (the outgoing ctx is g_arch_current) --
void arch_switch(struct arch_context* from, struct arch_context* to)
{
    (void)from; // PendSV saves g_arch_current; `from` is always that thread
    g_arch_next = to;
    reg32(SCB_ICSR) = ICSR_PENDSVSET;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}

int arch_in_isr(void)
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    // 9-bit exception-number mask (v7-M IPSR width). Correct on v6-M too: a
    // v6-M IPSR never exceeds 0x3F, so the wider mask yields the same result.
    return (ipsr & 0x1FF) != 0;
}

// --- One-shot timer (SysTick). Clock (arch_clock_now) is per-arch: chip-
// provided on v6-M (no DWT), a weak DWT default on v7-M. -----------------------
void arch_timer_arm(uint64_t deadline_ns)
{
    uint64_t now = arch_clock_now();
    uint64_t delta_ns = 0;
    if (deadline_ns > now)
    {
        delta_ns = deadline_ns - now;
    }
    // Clamp the delta to the one-shot range BEFORE converting, so a far-future
    // (or saturated UINT64_MAX) deadline can't overflow the ns*freq product. A
    // clamped deadline fires early and the kernel re-arms the remainder from
    // kickos_isr_timer (a harmless extra wake).
    // max_delta_ns depends only on the clock; cache it so a variable 64-bit divide
    // does not run on every arm (recomputed only when the clock changes at boot).
    uint64_t f = SystemCoreClock;
    static uint64_t cached_f = 0;
    static uint64_t cached_max_delta = ~0ull;
    if (f != cached_f)
    {
        if (f != 0)
        {
            cached_max_delta = (static_cast<uint64_t>(SYST_RVR_MAX) * 1_s) / f;
        }
        else
        {
            cached_max_delta = ~0ull;
        }
        cached_f = f;
    }
    uint64_t max_delta_ns = cached_max_delta;
    uint64_t cyc;
    if (delta_ns >= max_delta_ns)
    {
        cyc = SYST_RVR_MAX;
    }
    else
    {
        cyc = (delta_ns * f) / 1_s;
    }
    if (cyc == 0)
    {
        cyc = 1; // never program 0 (fires immediately / not at all)
    }
    reg32(SYST_CSR) = 0;                        // disable while reprogramming
    reg32(SCB_ICSR) = ICSR_PENDSTCLR;           // drop any pend latched while masked
    reg32(SYST_RVR) = static_cast<uint32_t>(cyc);
    reg32(SYST_CVR) = 0;                        // clear -> reload on next tick
    reg32(SYST_CSR) = SYST_CSR_CLKSOURCE | SYST_CSR_TICKINT | SYST_CSR_ENABLE;
}

void arch_timer_disarm(void)
{
    reg32(SYST_CSR) = 0;
    // "disarm" must mean no callback: clear a SysTick that pended while the line
    // was masked, else it fires once on lock release after we disarmed.
    reg32(SCB_ICSR) = ICSR_PENDSTCLR;
}

// --- MPU: hardware enforcement is M2 (item 12); no-op on M1 -----------------
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n)
{
    (void)regions;
    (void)n;
}

uintptr_t arch_ram_base(void)
{
    return reinterpret_cast<uintptr_t>(__kickos_ram_start);
}

size_t arch_ram_size(void)
{
    return static_cast<size_t>(__kickos_ram_end - __kickos_ram_start);
}

void* arch_ram_alloc(size_t size)
{
    size_t total = arch_ram_size();
    size_t need = (size + 31u) & ~static_cast<size_t>(31u); // 32-byte aligned
    arch_irq_state_t s = arch_irq_save();
    void* p = nullptr;
    if (need != 0 and need <= total - g_ram_used)
    {
        p = __kickos_ram_start + g_ram_used;
        g_ram_used += need;
    }
    arch_irq_restore(s);
    return p;
}

uintptr_t arch_mpu_probe_addr(void)
{
    return 0; // no MPU on M1 -> no probe address available
}

// --- Interrupt controller (NVIC) --------------------------------------------
void arch_irq_mask(int line)
{
    if (line < 0)
    {
        return;
    }
    unsigned l = static_cast<unsigned>(line);
    reg32(NVIC_ICER0 + (l >> 5) * 4) = 1u << (l & 31);
}

void arch_irq_inject(int irq)
{
    if (irq < 0)
    {
        return;
    }
    unsigned l = static_cast<unsigned>(irq);
    // Match the proven sim semantics: a masked (disabled) line drops the raise
    // rather than latching pending to fire on unmask.
    if ((reg32(NVIC_ISER0 + (l >> 5) * 4) & (1u << (l & 31))) == 0)
    {
        return;
    }
    reg32(NVIC_ISPR0 + (l >> 5) * 4) = 1u << (l & 31);
}

// --- Idle / halt ------------------------------------------------------------
// weak: real silicon halts (WFI) to save power. A QEMU semihosting-clock chip
// (mps2, nrf51) overrides this to SPIN: QEMU <= 10 freezes the semihosting
// SYS_CLOCK -- our monotonic clock there -- while the core is in WFI, so a timed
// sleep with every thread idle would never wake (the clock stops). QEMU 11 fixed
// it; spinning keeps the clock advancing on the older QEMU the CI runner ships.
void __attribute__((weak)) arch_idle_wait(void)
{
    __asm volatile("wfi");
}

// --- Kernel-facing ISR entries ----------------------------------------------
// SysTick expiry: disarm (one-shot tickless model) then run the kernel handler,
// which re-arms the next deadline via arch_timer_arm.
void SysTick_Handler(void)
{
    reg32(SYST_CSR) = 0;
    kickos_isr_timer();
}

}
