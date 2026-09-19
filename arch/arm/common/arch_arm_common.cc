// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Core-generic ARM Cortex-M arch backend: the parts of the arch.h seam whose
// implementation is IDENTICAL on ARMv6-M and ARMv7-M (deferred PendSV switch,
// SysTick one-shot timer, the whole NVIC line set, the default-IRQ entry, the
// wild-PSP and no-kernel-block reports, the idle wait, and the SysTick ISR
// entry). Compiled into BOTH kickos_arch_armv6m and kickos_arch_armv7m.
//
// What differs per profile arrives through <arm_isa.h>, one copy per ISA
// directory and one on the include path per archive.
//
// The arch-profile-specific edges stay in arch_armv{6,7}m.cc: context init (the
// v7-M frame carries an EXC_RETURN word), the critical section (PRIMASK vs
// BASEPRI), the fault reporter (v6-M has no fault-status registers at all), the
// monotonic/trace clock source, and per-arch bring-up.

#include <kickos/arch/arch.h>

#include <kickos/sys/atomic.h>


#include <kickos/units.h> // _s literal (== 1e9 ns) for the ns/cycle conversions

#include <arm_isa.h> // per-profile: the panic banners and the device-priority write

#include "regs.h"

#include <stddef.h>

// 0 keeps only the one-line fault marker.
#ifndef KICKOS_PANIC_DUMP
#define KICKOS_PANIC_DUMP 1
#endif

namespace
{
    using namespace kickos::arm;
    using namespace kickos::units; // _s == 1e9 ns

    using kickos::Atomic;
    using kickos::Order;
}

// kfault_terminate is the shared panic/fault dead-end (kernel.h).
namespace kickos
{
    void kprintf(char const* fmt, ...);
}
extern "C" void kpanic_enter(void);
extern "C" void kfault_terminate(void) __attribute__((noreturn));

extern "C"
{

    // CMSIS convention: the core clock in Hz, defined + maintained by the chip.
    extern uint32_t SystemCoreClock;

    // Shared with switch.S (the PendSV/arch_start switch targets): written by C
    // (arch_switch) and by asm (PendSV).
    Atomic<struct arch_context*, Order::RELAXED> g_arch_current = nullptr;
    Atomic<struct arch_context*, Order::RELAXED> g_arch_next = nullptr;

    // switch.S loads each as a plain word at offset 0. Nothing else enforces the layout.
    static_assert(sizeof(g_arch_current) == sizeof(struct arch_context*), "asm reads one word");
    static_assert(sizeof(g_arch_next) == sizeof(struct arch_context*), "asm reads one word");
    static_assert(alignof(decltype(g_arch_current)) == alignof(struct arch_context*), "asm reads it naturally aligned");
}

// ===========================================================================
extern "C"
{

// Anything file-local below is `static`, never an anonymous namespace: C language
// linkage overrides the namespace, so an anonymous namespace nested in this block
// emits an unmangled GLOBAL symbol.

// A trap prologue refused the running thread's live SP. Contain the thread instead of ending
// the system: the kernel slays it and hands back the context to run instead, which this seats
// for the restore half the caller in switch.S branches to. Returns 1 when contained, 0 when
// the caller must terminate.
int kickos_arm_contain_wild_sp(void)
{
    // g_arch_current, not the scheduler's current: this one tracks the PHYSICAL switch, so it
    // names the thread whose pointer was refused even when a booked switch has already
    // published another one as current.
    struct arch_context* const next = kickos_thread_contain_wild_stack(g_arch_current, nullptr);
    if (next == nullptr)
    {
        return 0;
    }
    // g_arch_current is written by the restore half itself, so only the target is set here.
    g_arch_next = next;
    return 1;
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

int arch_in_isr(void)
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    // 9-bit exception-number mask (v7-M IPSR width). Correct on v6-M too: a
    // v6-M IPSR never exceeds 0x3F, so the wider mask yields the same result.
    uint32_t const exc = ipsr & 0x1FFu;
    // 11 is SVCall. This predicate is asked whether a switch requested here would DEFER,
    // and inside the syscall trap it would not: the trap runs on behalf of the thread that
    // issued the SVC, and the IPC fastpath blocks and performs the switch there. Ungated by
    // the fastpath because no backend runs any C at all in SVCall without one, so no reader
    // can observe the difference, and a predicate that answered differently per build knob
    // would be a second truth.
    if (exc == 11u)
    {
        return 0;
    }
    return exc != 0;
}

uint64_t arch_cpu_clock_hz(void)
{
    return SystemCoreClock;
}

// --- One-shot timer (SysTick). Clock (arch_clock_now) is a per-CHIP contract on
// every ARM arch; there is no arch-level fallback. --------------------------
// RELOADS UNCONDITIONALLY, AND THE SKIP IS THE KERNEL'S. Blindly reloading SYST_CVR resets
// the countdown, so a far deadline reached only while lower-prio threads switch faster than
// it can expire (e.g. a bench reporter's 0.5 s sleep behind two CPU-bound players) would
// starve forever; what keeps that from happening is kickos::ktime_rearm skipping the call
// when the deadline it holds is already the one programmed.
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

// --- MPU ---------------------------------------------------------------------
// The stash every ARM backend reads, and the two apply entries, are their own archive
// member: arch/arm/common/arch_arm_mpu_pending.cc. The PMSAv7 descriptor writer is
// arch_arm_mpu_pmsav7.cc; SYSMPU and PMSAv8 program their own from their own commits.

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
    // AHEAD OF THE ENABLE, never after: on a profile that bands device lines, a line enabled
    // at the controller's reset priority preempts an IrqLock-held section until this lands.
    kickos_arm_irq_line_prio(l);
    // A pending bit latched while the line was masked survives the enable and fires the
    // instant ISER is set. The dsb drains a preceding device-flag clear, whose W1C may
    // still sit in the write buffer (exception entry does not order device writes), so a
    // level source that is genuinely deasserted does not re-latch.
    __asm volatile("dsb" ::: "memory");
    reg32(NVIC_ISER0 + (l >> 5) * 4) = 1u << (l & 31);
}

void arch_irq_clear_pending(int line)
{
    if (line < 0)
    {
        return;
    }
    unsigned l = static_cast<unsigned>(line);
    // Drain any pending device write before dropping the latched NVIC pending.
    __asm volatile("dsb" ::: "memory");
    reg32(NVIC_ICPR0 + (l >> 5) * 4) = 1u << (l & 31);
}

void arch_irq_inject(int irq)
{
    if (irq < 0)
    {
        return;
    }
    unsigned l = static_cast<unsigned>(irq);
    // Latch-and-coalesce: the NVIC holds ISPR pending independently of ISER, so a
    // raise on a masked (disabled) line latches and fires the instant the line is
    // enabled: write ISPR unconditionally, do not drop.
    reg32(NVIC_ISPR0 + (l >> 5) * 4) = 1u << (l & 31);
}

// --- Kernel-facing ISR entries ----------------------------------------------
// SysTick expiry: disarm (one-shot tickless model) then run the kernel handler,
// which re-arms the next deadline via arch_timer_arm.
void SysTick_Handler(void)
{
    reg32(SYST_CSR) = 0;
    kickos_isr_timer();
}

// The exception number in IPSR is 16 + the external line. The mask is the 9-bit v7-M IPSR
// width and is correct on v6-M too, whose IPSR never exceeds 0x3F (arch_in_isr above reads
// it the same way).
void kickos_arm_default_irq(void)
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    int line = static_cast<int>(ipsr & 0x1FFu) - 16;
    if (line >= 0)
    {
        kickos_isr_irq(line);
    }
}

// --- Refusals raised by switch.S --------------------------------------------
// From switch.S (PendSV and the SVC entry), when the running thread's live PSP lacks room
// BELOW it for the callee block about to be pushed there. Runs in handler mode on the MSP.
//
// RETURNS TO RESUME ANOTHER THREAD when the offending one could be contained; only the thread
// that chose the pointer dies. CONTAINS BEFORE REPORTING because kpanic_enter is one-way: it
// masks this core's IRQs and never restores them, so a system resumed after it has no timer.
void kickos_arm_bad_psp(uint32_t psp, uint32_t need, uint32_t lo, uint32_t hi)
{
#if KICKOS_KERNEL_STACKS
    int const contained = kickos_arm_contain_wild_sp();
#else
    // NO BLOCK TO REBUILD ON, so this board cannot contain: arch_ctx_redirect falls back to
    // fabricating the privileged exit frame on the thread's OWN USER STACK, which is the
    // write the guard above exists to refuse. A whole-board property (armv7m is the one arch
    // that resolves this knob to 0, on f302nucleo and bluepill-c8), so it is decided here and
    // not per thread, and the CONTAINED spelling below is compiled out with it.
    int const contained = 0;
#endif
    if (contained == 0)
    {
        kpanic_enter();
    }
#if KICKOS_PANIC_DUMP
    // Re-derived rather than passed: one guard serves all three legs.
    // THE NOUN IS THE DISCRIMINATOR. tests/lib/panic.ere matches "=== <ARCH> EXCEPTION", so a
    // contained refusal spelled that way is indistinguishable from one that ended the system
    // and assert_no_panic stops meaning anything on exactly the arms that now survive.
    char const* why = "no room below";
    if (psp < lo)
    {
        why = "under stack_lo";
    }
    else if (psp >= hi)
    {
        why = "at or above stack_hi";
    }
    // Which guarded push refused: nothing in the arguments separates the sites, so it comes
    // from ICSR.VECTACTIVE.
    uint32_t const vect = reg32(SCB_ICSR) & 0x1FFu;
    char const* site = "handler";
    if (vect == 11u)
    {
        site = "SVCall";
    }
    else if (vect == 14u)
    {
        site = "PendSV";
    }
#if KICKOS_KERNEL_STACKS
    if (contained == 0)
    {
        ::kickos::kprintf(KICKOS_ARM_BANNER_WILD_PSP_WHY, why);
    }
    else
    {
        ::kickos::kprintf(KICKOS_ARM_BANNER_WILD_PSP_WHY_CONTAINED, why);
    }
#else
    ::kickos::kprintf(KICKOS_ARM_BANNER_WILD_PSP_WHY, why);
#endif
    ::kickos::kprintf("  in %s PSP=0x%x need=%u stack=[0x%x,0x%x)\n", site,
                      static_cast<unsigned>(psp), static_cast<unsigned>(need),
                      static_cast<unsigned>(lo), static_cast<unsigned>(hi));
#else
    (void)psp;
    (void)need;
    (void)lo;
    (void)hi;
#if KICKOS_KERNEL_STACKS
    if (contained == 0)
    {
        ::kickos::kprintf(KICKOS_ARM_BANNER_WILD_PSP);
    }
    else
    {
        ::kickos::kprintf(KICKOS_ARM_BANNER_WILD_PSP_CONTAINED);
    }
#else
    ::kickos::kprintf(KICKOS_ARM_BANNER_WILD_PSP);
#endif
#endif
    if (contained == 0)
    {
        kfault_terminate();
    }
}

#if KICKOS_KERNEL_STACKS
// From svc_trampoline, when the calling thread has no kernel block seated. Runs privileged
// in THREAD mode ON THE MSP, .Lsvc_nokstack having cleared CONTROL.SPSEL before the branch;
// `psp` is the thread's own, computed before that clear.
void kickos_arm_no_kernel_stack(uint32_t psp)
{
    kpanic_enter();
    ::kickos::kprintf(KICKOS_ARM_BANNER_NO_KERNEL_STACK);
#if KICKOS_PANIC_DUMP
    ::kickos::kprintf("  in svc_trampoline PSP=0x%x\n", static_cast<unsigned>(psp));
#else
    (void)psp;
#endif
    kfault_terminate();
}
#endif

}
