// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ARMv7-M arch backend: the parts of the arch.h seam that are Cortex-M core
// generic (present on every v7-M part, chip-independent). The context switch +
// syscall trap assembly lives in switch.S; the chip layer (arch/arm/chip/*)
// supplies the truly hardware-specific edges -- arch_init (clocks + console +
// exception-priority install), arch_console_write (UART) -- and the linker
// script that defines the user-RAM region and SystemCoreClock.
//
// Runtime verification is pending a Cortex-M4 execution target (QEMU or K64F
// hardware); this compiles clean for the target ISA and the switch/SVC paths
// are validated by construction + disassembly (see docs/porting.md).

#include <kickos/arch/arch.h>

#include "regs.h"

#include <stddef.h> // offsetof

// switch.S hard-codes these arch_context field offsets; keep struct and asm in
// sync (a silent reorder would corrupt the saved SP / privilege state).
static_assert(offsetof(struct arch_context, sp) == 0, "switch.S expects ctx.sp @0");
static_assert(offsetof(struct arch_context, npriv) == 4, "switch.S expects ctx.npriv @4");
static_assert(offsetof(struct arch_context, resting_npriv) == 8,
              "switch.S expects ctx.resting_npriv @8");

namespace
{
    using namespace kickos::armv7m;

    // The DWT cycle counter is 32-bit; extend it to a monotonic 64-bit cycle
    // count in software by catching wraps on each read. LIMITATION (M1): a wrap
    // that is not observed within one 2^32-cycle period (~35 s at 120 MHz) is
    // missed. A DWT/timer overflow interrupt is the refinement (item 10/12a).
    volatile uint32_t g_cyc_high = 0;
    volatile uint32_t g_cyc_last = 0;

    // User-RAM region for arch_ram_alloc, defined by the chip linker script.
    // Bump-allocated; freed only wholesale (matches the sim arena's M0 model).
    volatile uint32_t g_ram_used = 0;
}

extern "C"
{
    // Userspace thread epilogue (kickos_user): an unprivileged thread whose entry
    // returns cannot run the kernel's kickos_thread_return directly (it would
    // execute exit_current with nPRIV=1 -> IrqLock/BASEPRI is a no-op and the
    // SCS write in arch_switch BusFaults). It must trap out via the exit syscall.
    void kickos_user_thread_return(void);

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

namespace
{
    inline uint64_t now_cycles()
    {
        // Called from thread and ISR context; the wrap-extend read must be
        // atomic against a concurrent reader, so run it under the crit section.
        arch_irq_state_t s = arch_irq_save();
        uint32_t cur = reg32(DWT_CYCCNT);
        if (cur < g_cyc_last)
        {
            g_cyc_high++;
        }
        g_cyc_last = cur;
        uint64_t hi = g_cyc_high;
        arch_irq_restore(s);
        return (hi << 32) | cur;
    }

    inline uint64_t cycles_to_ns(uint64_t cyc)
    {
        uint64_t f = SystemCoreClock;
        if (f == 0)
        {
            return 0;
        }
        // Split to avoid overflow: sec*1e9 + (rem*1e9)/f, rem < f.
        uint64_t sec = cyc / f;
        uint64_t rem = cyc % f;
        return sec * 1000000000ull + (rem * 1000000000ull) / f;
    }

    inline uint64_t ns_to_cycles(uint64_t ns)
    {
        uint64_t f = SystemCoreClock;
        return (ns * f) / 1000000000ull;
    }
}

// ===========================================================================
extern "C"
{

// --- Context init: fabricate a first-switch-in frame (see switch.S layout) --
void arch_context_init(struct arch_context* ctx,
                       void (*entry)(void*), void* arg,
                       void* stack_base, size_t stack_size,
                       int privileged)
{
    uintptr_t top = reinterpret_cast<uintptr_t>(stack_base) + stack_size;
    top &= ~static_cast<uintptr_t>(7); // AAPCS: 8-byte aligned stack
    uint32_t* sp = reinterpret_cast<uint32_t*>(top);

    // A privileged (kernel) thread returns straight into the kernel epilogue; an
    // unprivileged thread must reach the exit path via a syscall trap (see the
    // kickos_user_thread_return note above).
    uint32_t ret = reinterpret_cast<uint32_t>(kickos_thread_return);
    if (not privileged)
    {
        ret = reinterpret_cast<uint32_t>(kickos_user_thread_return);
    }

    // Hardware exception frame (unstacked by the exception return into `entry`).
    *(--sp) = 0x01000000u;                                    // xPSR (Thumb bit)
    *(--sp) = reinterpret_cast<uint32_t>(entry) & ~1u;        // PC = entry
    *(--sp) = ret;                                            // LR: entry returns here
    *(--sp) = 0;                                              // r12
    *(--sp) = 0;                                              // r3
    *(--sp) = 0;                                              // r2
    *(--sp) = 0;                                              // r1
    *(--sp) = reinterpret_cast<uint32_t>(arg);               // r0 = arg

    // PendSV-saved block: {r4-r11, EXC_RETURN}, popped by ldmia (r4 lowest,
    // EXC_RETURN highest), so push EXC_RETURN first.
    *(--sp) = 0xFFFFFFFDu; // EXC_RETURN: thread mode, PSP, non-FP frame
    for (int i = 0; i < 8; i++)
    {
        *(--sp) = 0; // r11..r4
    }

    ctx->sp = reinterpret_cast<uint32_t>(sp);
    // CONTROL.nPRIV: 0 = privileged, 1 = unprivileged. Seed both the live
    // (saved/restored) value and the fixed resting value.
    uint32_t npriv = 1;
    if (privileged)
    {
        npriv = 0;
    }
    ctx->npriv = npriv;
    ctx->resting_npriv = npriv;
}

// --- Switch: always deferred to PendSV (the outgoing ctx is g_arch_current) --
void arch_switch(struct arch_context* from, struct arch_context* to)
{
    (void)from; // PendSV saves g_arch_current; `from` is always that thread
    g_arch_next = to;
    reg32(SCB_ICSR) = ICSR_PENDSVSET;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}

// --- Critical section: raise BASEPRI to the kernel lock threshold -----------
arch_irq_state_t arch_irq_save(void)
{
    uint32_t prev;
    uint32_t lock = PRIO_LOCK_BASEPRI;
    __asm volatile("mrs %0, basepri" : "=r"(prev));
    __asm volatile("msr basepri, %0" ::"r"(lock) : "memory");
    // Raising BASEPRI is not self-synchronizing: without these barriers an
    // interrupt could be taken on the following instruction under the OLD mask
    // (ARMv7-M ARM: a BASEPRI write needs DSB+ISB to take effect).
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
    return prev;
}

void arch_irq_restore(arch_irq_state_t state)
{
    __asm volatile("msr basepri, %0" ::"r"(state) : "memory");
}

int arch_in_isr(void)
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    return (ipsr & 0x1FF) != 0;
}

// --- Tickless clock (DWT) + one-shot timer (SysTick) ------------------------
// weak: the monotonic clock SOURCE is chip-specific (the arch.h note: "free-
// running TIM/DWT + compare (or SysTick)"). DWT is the default for real silicon;
// a chip whose DWT is absent/unimplemented (e.g. QEMU) overrides this.
uint64_t __attribute__((weak)) arch_clock_now(void)
{
    return cycles_to_ns(now_cycles());
}

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
        cyc = ns_to_cycles(delta_ns);
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
    // was BASEPRI-masked, else it fires once on lock release after we disarmed.
    reg32(SCB_ICSR) = ICSR_PENDSTCLR;
}

// --- MPU: hardware enforcement is M2 (item 12); no-op on M1 -----------------
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n)
{
    (void)regions;
    (void)n;
    // M1 is privilege + SVC only. Per-task MPU region programming (K64F SYSMPU /
    // v7-M PMSA) lands in M2; until then no memory isolation is enforced on the
    // target (matches the roadmap: "MCUs first come up with privilege + SVC").
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
    // No MPU on M1 -> no address faults on unprivileged access. The isolation
    // self-test's fault stage is a no-op here (roadmap: "minus the enforced-
    // MPU-fault case"). Return 0 to signal "no probe address available".
    return 0;
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

void arch_irq_unmask(int line)
{
    if (line < 0)
    {
        return;
    }
    unsigned l = static_cast<unsigned>(line);
    // Program the line's priority into the kernel-maskable band BEFORE enabling
    // it: NVIC IPR resets to 0x00, and the BASEPRI (0x20) critical section only
    // masks priorities numerically >= 0x20. Without this a device IRQ would
    // preempt an IrqLock-held section and corrupt kernel state (regs.h band).
    reinterpret_cast<volatile uint8_t*>(NVIC_IPR0)[l] = static_cast<uint8_t>(PRIO_DEVICE);
    reg32(NVIC_ISER0 + (l >> 5) * 4) = 1u << (l & 31);
}

void arch_irq_inject(int irq)
{
    if (irq < 0)
    {
        return;
    }
    unsigned l = static_cast<unsigned>(irq);
    // Match the proven sim semantics: a masked (disabled) line drops the raise
    // rather than latching pending to fire on unmask. The driver re-arms by
    // unmasking at irq_ack (item 11a revisits this on real silicon).
    if ((reg32(NVIC_ISER0 + (l >> 5) * 4) & (1u << (l & 31))) == 0)
    {
        return;
    }
    reg32(NVIC_ISPR0 + (l >> 5) * 4) = 1u << (l & 31);
}

// --- Idle / halt ------------------------------------------------------------
void arch_idle_wait(void)
{
    __asm volatile("wfi");
}

// arch_shutdown is chip-specific (a real MCU halts; QEMU exits via semihosting),
// so it lives in the chip backend, not here.

// --- Kernel-facing ISR entries ----------------------------------------------
// SysTick expiry: disarm (one-shot tickless model) then run the kernel handler,
// which re-arms the next deadline via arch_timer_arm.
void SysTick_Handler(void)
{
    reg32(SYST_CSR) = 0;
    kickos_isr_timer();
}

// Common external-IRQ entry: the chip vector table routes NVIC lines here. The
// exception number in IPSR is 16 + external-line, so the line is IPSR - 16.
void kickos_armv7m_default_irq(void)
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    int line = static_cast<int>(ipsr & 0x1FF) - 16;
    if (line >= 0)
    {
        kickos_isr_irq(line);
    }
}

// --- One-time core bring-up, called by the chip's arch_init -----------------
// Installs the system-handler priorities the BASEPRI crit section depends on and
// starts the DWT cycle counter that backs the monotonic clock.
void kickos_armv7m_init(void)
{
    // SHPR2[31:24] = SVCall (#11); SHPR3[23:16] = PendSV (#14), [31:24] = SysTick.
    reg32(SCB_SHPR2) = (reg32(SCB_SHPR2) & 0x00FFFFFFu) | (PRIO_SVCALL << 24);
    uint32_t shpr3 = reg32(SCB_SHPR3) & 0x0000FFFFu;
    shpr3 |= (PRIO_PENDSV << 16) | (PRIO_SYSTICK << 24);
    reg32(SCB_SHPR3) = shpr3;

    // Enable the DWT cycle counter (monotonic clock source).
    reg32(DCB_DEMCR) |= DEMCR_TRCENA;
    reg32(DWT_CYCCNT) = 0;
    reg32(DWT_CTRL) |= DWT_CTRL_CYCCNTENA;
    g_cyc_high = 0;
    g_cyc_last = 0;
}

}
