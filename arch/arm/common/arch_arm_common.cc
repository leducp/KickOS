// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Core-generic ARM Cortex-M arch backend: the parts of the arch.h seam whose
// implementation is IDENTICAL on ARMv6-M and ARMv7-M (deferred PendSV switch,
// SysTick one-shot timer, NVIC mask/unmask/inject, the idle wait, and the
// SysTick ISR entry). Compiled into BOTH kickos_arch_armv6m
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

}

extern "C"
{

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
// Absolute deadline the running SysTick was last programmed for (UINT64_MAX ==
// disarmed / fired). ktime_rearm() calls arch_timer_arm on EVERY reschedule with
// the same pending deadline; blindly reloading SYST_CVR each time resets the
// countdown, so a far deadline reached only while lower-prio threads switch
// faster than it can expire (e.g. a bench reporter's 0.5 s sleep behind two
// CPU-bound players) starves forever. Guard: if the same deadline is already
// counting (SysTick still enabled), leave it running. The one-shot ISR disables
// SysTick before it re-arms, so its own re-arm (this exact deadline, remainder of
// a clamped wait) is never skipped. Touched only under the kernel IrqLock.
static uint64_t g_armed_deadline_ns = ~0ull;

void arch_timer_arm(uint64_t deadline_ns)
{
    if (deadline_ns == g_armed_deadline_ns and (reg32(SYST_CSR) & SYST_CSR_ENABLE) != 0)
    {
        return;
    }
    g_armed_deadline_ns = deadline_ns;
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
    g_armed_deadline_ns = ~0ull;
    reg32(SYST_CSR) = 0;
    // "disarm" must mean no callback: clear a SysTick that pended while the line
    // was masked, else it fires once on lock release after we disarmed.
    reg32(SCB_ICSR) = ICSR_PENDSTCLR;
}

// --- MPU: ARM PMSA backend (v6-M/v7-M share the register map) ----------------
#if KICKOS_HAVE_MPU
namespace
{
    // Encode one region into an MPU_RASR value. attr is the UNPRIVILEGED access
    // (supervisor comes from the PRIVDEFENA background region): a code region is
    // R+X (RO, executable); data / stack / device is RW + execute-never. Device
    // memory gets the ordered device type. `size` is a power of two >= 32, and the
    // region base is naturally aligned to it (arch_ram_region_size / the linker).
    uint32_t mpu_rasr(size_t size, uint32_t attr)
    {
        using namespace kickos::arm;
        uint32_t const size_field =
            static_cast<uint32_t>(__builtin_ctz(static_cast<unsigned>(size))) - 1u;
        uint32_t rasr = MPU_RASR_ENABLE | (size_field << 1);
        if (attr & ARCH_MPU_X)
        {
            rasr |= MPU_RASR_AP_RO | MPU_RASR_MEM_NORMAL; // code: RO + executable
        }
        else if (attr & ARCH_MPU_DEV)
        {
            rasr |= MPU_RASR_AP_RW | MPU_RASR_XN | MPU_RASR_MEM_DEVICE; // MMIO
        }
        else
        {
            rasr |= MPU_RASR_AP_RW | MPU_RASR_XN | MPU_RASR_MEM_NORMAL; // data/stack
        }
        return rasr;
    }
}

void __attribute__((weak)) arch_mpu_apply(struct arch_mpu_region const* regions, size_t n)
{
    using namespace kickos::arm;
    // A domain switch / privilege change can only take effect atomically, so
    // disable the MPU while reprogramming, then re-enable with the PRIVDEFENA
    // background (privileged code keeps the default map; unprivileged code sees
    // only the programmed regions). MEMFAULTENA makes a violation a clean
    // MemManage rather than an escalated HardFault.
    reg32(SCB_SHCSR) |= SHCSR_MEMFAULTENA;
    reg32(MPU_CTRL) = 0;
    __asm volatile("dsb" ::: "memory");
    // Program regions[0..n), disable the rest up to the hardware descriptor count
    // (MPU_TYPE.DREGION) -- so a thread with fewer regions than the last one leaves
    // no stale window enabled.
    size_t const hw_regions = (reg32(MPU_TYPE) >> 8) & 0xFFu;
    for (size_t i = 0; i < hw_regions; i++)
    {
        reg32(MPU_RNR) = static_cast<uint32_t>(i);
        // PMSA requires a power-of-two size with a naturally-aligned base. Encode
        // ONLY such regions; a non-pow2 region is fail-closed (descriptor disabled),
        // never silently mis-encoded (ctz would under-size it and the base would
        // snap). Unprivileged regions are pow2 by construction (arch_ram_region_*
        // + the pow2 linker code/data sections); a privileged thread's non-pow2
        // whole-arena grant is simply dropped here -- harmless, it runs on the
        // PRIVDEFENA background. (Linker contract: code/data regions must be pow2.)
        if (i < n and regions[i].size >= 32
            and (regions[i].size & (regions[i].size - 1)) == 0)
        {
            reg32(MPU_RBAR) = static_cast<uint32_t>(regions[i].base) & ~0x1Fu;
            reg32(MPU_RASR) = mpu_rasr(regions[i].size, regions[i].attr);
        }
        else
        {
            reg32(MPU_RASR) = 0; // unused / non-pow2: disable the descriptor
        }
    }
    __asm volatile("dsb" ::: "memory");
    reg32(MPU_CTRL) = MPU_CTRL_ENABLE | MPU_CTRL_PRIVDEFENA;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}
#else
// No enforcement on this board (KICKOS_HAVE_MPU=0): privilege + SVC only.
void __attribute__((weak)) arch_mpu_apply(struct arch_mpu_region const* regions, size_t n)
{
    (void)regions;
    (void)n;
}
#endif

size_t __attribute__((weak)) arch_mpu_min_region(void)
{
    return 32u; // ARMv6-M / v7-M PMSA min region; a no-MPU chip (nRF51) overrides to 0
}

// PMSA needs a power-of-two size >= 32 with the base naturally aligned to it (the
// RBAR base masking in arch_mpu_apply assumes exactly that). K64F (SYSMPU, byte-
// granular) overrides this strongly; a no-MPU ARM chip (min 0) falls to the
// 16-byte-granular branch.
bool __attribute__((weak)) arch_mpu_region_encodable(uintptr_t base, size_t size)
{
    if (size == 0)
    {
        return false;
    }
    size_t const min = arch_mpu_min_region();
    if (min == 0)
    {
        return (base & 15u) == 0 and (size & 15u) == 0;
    }
    if (size < min or (size & (size - 1)) != 0)
    {
        return false;
    }
    return (base & (size - 1)) == 0;
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
