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
// are validated by construction + disassembly (see docs/reference/porting.md).

#include <kickos/arch/arch.h>
#include <kickos/units.h> // _s literal (== 1e9 ns) for the cycle<->ns conversions

#include "regs.h"

#include <stddef.h> // offsetof

// switch.S hard-codes these arch_context field offsets; keep struct and asm in
// sync (a silent reorder would corrupt the saved SP / privilege state).
static_assert(offsetof(struct arch_context, sp) == 0, "switch.S expects ctx.sp @0");
static_assert(offsetof(struct arch_context, npriv) == 4, "switch.S expects ctx.npriv @4");
static_assert(offsetof(struct arch_context, resting_npriv) == 8,
              "switch.S expects ctx.resting_npriv @8");
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
static_assert(offsetof(struct arch_context, trace_tid) == 12,
              "switch.S telemetry hook expects ctx.trace_tid @12");
#endif

namespace
{
    using namespace kickos::arm;    // reg32 (shared core regs)
    using namespace kickos::armv7m; // BASEPRI band, DWT_*, SHPR (arch-specific)
}

extern "C"
{
    // Userspace thread epilogue (kickos_user): an unprivileged thread whose entry
    // returns cannot run the kernel's kickos_thread_return directly (it would
    // execute exit_current with nPRIV=1 -> IrqLock/BASEPRI is a no-op and the
    // SCS write in arch_switch BusFaults). It must trap out via the exit syscall.
    void kickos_user_thread_return(void);

    // CMSIS convention: the core clock in Hz, defined + maintained by the chip.
    extern uint32_t SystemCoreClock;
}

namespace
{
    using namespace kickos::units; // _s == 1e9 ns

    inline uint64_t ns_to_cycles(uint64_t ns)
    {
        uint64_t f = SystemCoreClock;
        return (ns * f) / 1_s;
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

// --- Critical section: raise BASEPRI to the kernel lock threshold -----------
arch_irq_state_t arch_irq_save(void)
{
    uint32_t prev;
    __asm volatile("mrs %0, basepri" : "=r"(prev));
    // Nested-lock fast path: if BASEPRI already masks at least as strongly as the
    // lock, the section is already in effect -- no BASEPRI change and thus no barrier
    // is needed. Skips the DSB+ISB (a pipeline flush) on every nested IrqLock; the hot
    // syscall->sem->wake->reschedule->ktime_rearm path nests ~6-8. Lower BASEPRI value
    // = stronger mask; 0 = no mask. (A weaker prev, e.g. a device band 0x30, still
    // raises to the lock below.)
    if (prev != 0 and prev <= PRIO_LOCK_BASEPRI)
    {
        return prev;
    }
    __asm volatile("msr basepri, %0" ::"r"(PRIO_LOCK_BASEPRI) : "memory");
    // Raising BASEPRI is not self-synchronizing: without these barriers an
    // interrupt could be taken on the following instruction under the OLD mask
    // (ARMv7-M ARM, "Barriers" -- a BASEPRI write needs DSB+ISB to take effect).
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
    return prev;
}

void arch_irq_restore(arch_irq_state_t state)
{
    __asm volatile("msr basepri, %0" ::"r"(state) : "memory");
}

// --- Monotonic clock: NO armv7m default -------------------------------------
// arch_clock_now is a REQUIRED chip contract (a strong per-chip definition over a
// dedicated peripheral timer). There is deliberately no weak DWT fallback here: the
// DWT is debug-domain (gated by DEMCR.TRCENA, lockable on Cortex-M7, absent under
// QEMU) and every chip that ever relied on the old fallback hit a broken clock, so a
// board that forgets to provide one must fail LOUD at link time, not hang on its
// first sleep. The one-shot SysTick timer is core-generic (arch_arm_common).

// weak: telemetry trace clock = the raw DWT cycle counter (32-bit, wraps). Cycle-
// accurate on real silicon and already running (kickos_armv7m_init enabled it).
// A part whose DWT is frozen/absent (QEMU mps2) overrides this, like arch_clock_now.
uint32_t __attribute__((weak)) arch_trace_now(void)
{
    return reg32(DWT_CYCCNT);
}

// --- Interrupt controller (NVIC). mask/inject are core-generic (arm/common);
// only unmask is arch-specific: it programs the BASEPRI-maskable priority band
// the crit section relies on, which v6-M (PRIMASK-masks-all) has no analogue for.
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
    // Latch-and-coalesce: PRESERVE any latched NVIC pending across enable -- a raise
    // that arrived while the line was masked fires through the normal ISR path the
    // instant ISER is set. Drain a preceding device-flag clear (dsb -- the W1C may
    // still sit in the write buffer, and exception entry does not order device
    // writes) so a level source that is genuinely deasserted does not re-latch.
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
    // Drain any pending device write, then drop the latched NVIC pending (ICPR).
    __asm volatile("dsb" ::: "memory");
    reg32(NVIC_ICPR0 + (l >> 5) * 4) = 1u << (l & 31);
}

// arch_shutdown is chip-specific (a real MCU halts; QEMU exits via semihosting),
// so it lives in the chip backend, not here.

// --- Kernel-facing ISR entries ----------------------------------------------
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

// --- Fault reporting: a shared HardFault (+ MemManage/BusFault/UsageFault, which
// the chip vectors route here too) that dumps the CPU context before the dead-end.
// Replaces the per-chip stubs that discarded everything. -----------------------
#ifndef KICKOS_PANIC_DUMP
#define KICKOS_PANIC_DUMP 1
#endif
}

namespace kickos
{
    void kprintf(char const* fmt, ...);
}
extern "C" void kpanic_enter(void);
extern "C" void kfault_terminate(void) __attribute__((noreturn));

extern "C"
{

// C side of the fault handler: `frame` points at the hardware-stacked exception
// frame {r0,r1,r2,r3,r12,lr,pc,xPSR}; `exc_return` is the EXC_RETURN in LR (bit 2
// selects the pre-fault stack). Dump it plus the fault-status registers, then hand
// off to the shared terminal (blink on real HW, exit on host/QEMU).
void kickos_armv7m_fault_report(uint32_t* frame, uint32_t exc_return)
{
    kpanic_enter(); // mask IRQs + force the sync path + flush queued bytes, in order
#if KICKOS_PANIC_DUMP
    uint32_t cfsr = kickos::arm::reg32(0xE000ED28);
    uint32_t hfsr = kickos::arm::reg32(0xE000ED2C);
    char const* stk = "MSP";
    if (exc_return & 0x4u)
    {
        stk = "PSP";
    }
    // A set MMFSR byte (CFSR[7:0]) means the MemManage (MPU) fault took it -- label
    // it as such so an isolation trap reads clearly; otherwise the generic label.
    char const* label = "HARD FAULT";
    if (cfsr & 0xFFu)
    {
        label = "MPU FAULT";
    }
    ::kickos::kprintf("\n=== %s ===\n", label);
    ::kickos::kprintf("  PC=0x%x LR=0x%x xPSR=0x%x (%s)\n",
                      frame[6], frame[5], frame[7], stk);
    ::kickos::kprintf("  R0=0x%x R1=0x%x R2=0x%x R3=0x%x R12=0x%x\n",
                      frame[0], frame[1], frame[2], frame[3], frame[4]);
    ::kickos::kprintf("  CFSR=0x%x HFSR=0x%x\n", cfsr, hfsr);
    if (cfsr & (1u << 10)) // BFSR IMPRECISERR: the stacked PC is past the faulting store
    {
        ::kickos::kprintf("  (imprecise bus fault: PC/regs are post-fault, not the culprit)\n");
    }
    // MMFAR/BFAR only hold a valid address when the matching CFSR VALID bit is set
    // (MMARVALID = bit 7, BFARVALID = bit 15); otherwise their contents are stale.
    if (cfsr & (1u << 7))
    {
        ::kickos::kprintf("  MMFAR=0x%x\n", kickos::arm::reg32(0xE000ED34));
    }
    if (cfsr & (1u << 15))
    {
        ::kickos::kprintf("  BFAR=0x%x\n", kickos::arm::reg32(0xE000ED38));
    }
    arch_fault_report_extra(); // chip hook: e.g. K64F SYSMPU error capture
#else
    (void)frame;
    (void)exc_return;
    ::kickos::kprintf("\n=== HARD FAULT ===\n");
#endif
    kfault_terminate();
}

// WEAK no-op: a chip whose isolation trap does not surface in the core CFSR
// (K64F SYSMPU) strong-overrides this to add its own capture (arch.h).
void __attribute__((weak)) arch_fault_report_extra(void)
{
}

// Naked entry: choose the stacked frame (MSP vs PSP per EXC_RETURN bit 2) and pass
// it, with EXC_RETURN, to the C reporter. Naked so no prologue perturbs the SP
// before we read it. The chip vector tables point HardFault/MemManage/BusFault/
// UsageFault all here.
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        "tst lr, #4          \n"
        "ite eq              \n"
        "mrseq r0, msp       \n"
        "mrsne r0, psp       \n"
        "mov r1, lr          \n"
        "b kickos_armv7m_fault_report \n");
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

    // Enable the DWT cycle counter (telemetry trace timestamp source; arch_trace_now).
    reg32(DCB_DEMCR) |= DEMCR_TRCENA;
    reg32(DWT_CYCCNT) = 0;
    reg32(DWT_CTRL) |= DWT_CTRL_CYCCNTENA;
}

}
