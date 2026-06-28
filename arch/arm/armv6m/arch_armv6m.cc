// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ARMv6-M (Cortex-M0/M0+) arch backend: the Cortex-M0 core-generic half of the
// arch.h seam. Context switch + syscall trap are in switch.S. Versus armv7m:
// the critical section is PRIMASK (mask ALL configurable interrupts -- v6-M has
// no BASEPRI), and there is no DWT cycle counter, so arch_clock_now is supplied
// by the chip (like arch_console_write), not here.
//
// Runtime-validated on QEMU Cortex-M0 (microbit); see docs/porting.md.

#include <kickos/arch/arch.h>

#include "regs.h"

#include <stddef.h> // offsetof

static_assert(offsetof(struct arch_context, sp) == 0, "switch.S expects ctx.sp @0");
static_assert(offsetof(struct arch_context, npriv) == 4, "switch.S expects ctx.npriv @4");
static_assert(offsetof(struct arch_context, resting_npriv) == 8,
              "switch.S expects ctx.resting_npriv @8");

namespace
{
    using namespace kickos::armv6m;

    volatile uint32_t g_ram_used = 0;
}

extern "C"
{
    extern unsigned char __kickos_ram_start[];
    extern unsigned char __kickos_ram_end[];

    // CMSIS convention; defined by the chip.
    extern uint32_t SystemCoreClock;

    // Chip-provided monotonic clock (v6-M has no DWT). Declared here because
    // arch_timer_arm needs "now" to compute the SysTick delta.
    uint64_t arch_clock_now(void);

    // Shared with switch.S.
    struct arch_context* volatile g_arch_current = nullptr;
    struct arch_context* volatile g_arch_next = nullptr;
}

// ===========================================================================
extern "C"
{

void arch_context_init(struct arch_context* ctx,
                       void (*entry)(void*), void* arg,
                       void* stack_base, size_t stack_size,
                       int privileged)
{
    void kickos_user_thread_return(void);

    uintptr_t top = reinterpret_cast<uintptr_t>(stack_base) + stack_size;
    top &= ~static_cast<uintptr_t>(7);
    uint32_t* sp = reinterpret_cast<uint32_t*>(top);

    uint32_t ret = reinterpret_cast<uint32_t>(kickos_thread_return);
    if (not privileged)
    {
        ret = reinterpret_cast<uint32_t>(kickos_user_thread_return);
    }

    // Hardware exception frame.
    *(--sp) = 0x01000000u;                              // xPSR (Thumb bit)
    *(--sp) = reinterpret_cast<uint32_t>(entry) & ~1u;  // PC = entry
    *(--sp) = ret;                                      // LR: entry returns here
    *(--sp) = 0;                                        // r12
    *(--sp) = 0;                                        // r3
    *(--sp) = 0;                                        // r2
    *(--sp) = 0;                                        // r1
    *(--sp) = reinterpret_cast<uint32_t>(arg);          // r0 = arg
    // PendSV-saved block {r4-r11} (no EXC_RETURN word on v6-M).
    for (int i = 0; i < 8; i++)
    {
        *(--sp) = 0;
    }

    ctx->sp = reinterpret_cast<uint32_t>(sp);
    uint32_t npriv = 1;
    if (privileged)
    {
        npriv = 0;
    }
    ctx->npriv = npriv;
    ctx->resting_npriv = npriv;
}

void arch_switch(struct arch_context* from, struct arch_context* to)
{
    (void)from;
    g_arch_next = to;
    reg32(SCB_ICSR) = ICSR_PENDSVSET;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}

// --- Critical section: PRIMASK (mask all configurable interrupts) -----------
arch_irq_state_t arch_irq_save(void)
{
    uint32_t prev;
    __asm volatile("mrs %0, primask" : "=r"(prev));
    __asm volatile("cpsid i" ::: "memory");
    return prev;
}

void arch_irq_restore(arch_irq_state_t state)
{
    // Restore the prior PRIMASK: if it was already set (nested lock), stay masked.
    __asm volatile("msr primask, %0" ::"r"(state) : "memory");
}

int arch_in_isr(void)
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    return (ipsr & 0x3F) != 0;
}

// --- One-shot timer (SysTick). Clock (arch_clock_now) is chip-provided. -----
void arch_timer_arm(uint64_t deadline_ns)
{
    uint64_t now = arch_clock_now();
    uint64_t delta_ns = 0;
    if (deadline_ns > now)
    {
        delta_ns = deadline_ns - now;
    }
    uint64_t f = SystemCoreClock;
    uint64_t max_delta_ns = ~0ull;
    if (f != 0)
    {
        max_delta_ns = (static_cast<uint64_t>(SYST_RVR_MAX) * 1000000000ull) / f;
    }
    uint64_t cyc;
    if (delta_ns >= max_delta_ns)
    {
        cyc = SYST_RVR_MAX;
    }
    else
    {
        cyc = (delta_ns * f) / 1000000000ull;
    }
    if (cyc == 0)
    {
        cyc = 1;
    }
    reg32(SYST_CSR) = 0;
    reg32(SCB_ICSR) = ICSR_PENDSTCLR;
    reg32(SYST_RVR) = static_cast<uint32_t>(cyc);
    reg32(SYST_CVR) = 0;
    reg32(SYST_CSR) = SYST_CSR_CLKSOURCE | SYST_CSR_TICKINT | SYST_CSR_ENABLE;
}

void arch_timer_disarm(void)
{
    reg32(SYST_CSR) = 0;
    reg32(SCB_ICSR) = ICSR_PENDSTCLR;
}

// --- MPU: M2 (no-op on M1) --------------------------------------------------
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
    size_t need = (size + 31u) & ~static_cast<size_t>(31u);
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
    return 0; // no MPU on M1
}

// --- Interrupt controller (NVIC). No priority band: the crit section is
// PRIMASK (masks all), so device-IRQ priorities don't gate it as on v7-M. -----
void arch_irq_mask(int line)
{
    if (line < 0)
    {
        return;
    }
    unsigned l = static_cast<unsigned>(line);
    reg32(NVIC_ICER0 + (l >> 5) * 4) = 1u << (l & 31);
}

void arch_irq_unmask(int line)
{
    if (line < 0)
    {
        return;
    }
    unsigned l = static_cast<unsigned>(line);
    reg32(NVIC_ISER0 + (l >> 5) * 4) = 1u << (l & 31);
}

void arch_irq_inject(int irq)
{
    if (irq < 0)
    {
        return;
    }
    unsigned l = static_cast<unsigned>(irq);
    if ((reg32(NVIC_ISER0 + (l >> 5) * 4) & (1u << (l & 31))) == 0)
    {
        return; // masked -> drop (matches sim semantics)
    }
    reg32(NVIC_ISPR0 + (l >> 5) * 4) = 1u << (l & 31);
}

void arch_idle_wait(void)
{
    __asm volatile("wfi");
}

// --- Kernel-facing ISR entries ----------------------------------------------
void SysTick_Handler(void)
{
    reg32(SYST_CSR) = 0;
    kickos_isr_timer();
}

void kickos_armv6m_default_irq(void)
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    int line = static_cast<int>(ipsr & 0x3F) - 16;
    if (line >= 0)
    {
        kickos_isr_irq(line);
    }
}

// Install the system-handler priorities (SHPR is word-access only on v6-M).
// No DWT to enable. Called by the chip's arch_init.
void kickos_armv6m_init(void)
{
    reg32(SCB_SHPR2) = (reg32(SCB_SHPR2) & 0x00FFFFFFu) | (PRIO_SVCALL << 24);
    uint32_t shpr3 = reg32(SCB_SHPR3) & 0x0000FFFFu;
    shpr3 |= (PRIO_PENDSV << 16) | (PRIO_SYSTICK << 24);
    reg32(SCB_SHPR3) = shpr3;
}

}
