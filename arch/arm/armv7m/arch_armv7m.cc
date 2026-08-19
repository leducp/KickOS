// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ARMv7-M arch backend: the parts of the arch.h seam that are Cortex-M core
// generic (present on every v7-M part, chip-independent). The context switch +
// syscall trap assembly lives in switch.S; the chip layer (arch/arm/chip/*)
// supplies the truly hardware-specific edges (arch_init: clocks + console +
// exception-priority install; arch_console_write: UART) and the linker script
// that defines the user-RAM region and SystemCoreClock.

#include <kickos/arch/arch.h>
#include <kickos/diag.h>
#include <kickos/units.h> // _s literal (== 1e9 ns) for the cycle<->ns conversions

#include "regs.h"
#include <kickos/trace/record.h> // ArchId: pin this build's trace-arch id to this backend

#include <stddef.h> // offsetof

// The trace-arch id (CMake ladder / this chip's caps.cmake) must equal the ArchId
// for the arch this backend implements, or a SESSION record mislabels the trace.
// A wrong caps.cmake value breaks the build here instead of drifting silently.
static_assert(KICKOS_TRACE_ARCH == kickos::trace::ARCH_ARMV7M,
              "KICKOS_TRACE_ARCH does not match ArchId::ARCH_ARMV7M for armv7m");

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
#if defined(KICKOS_ARCH_HAS_IPC_FASTPATH) && KICKOS_ARCH_HAS_IPC_FASTPATH
static_assert(kickos::armv7m::PRIO_LOCK_BASEPRI == 0x20,
              "SVC_Handler's fastpath raises this level as a literal");
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

#if defined(KICKOS_ARCH_HAS_IPC_FASTPATH) && KICKOS_ARCH_HAS_IPC_FASTPATH
// The fastpath parks a caller on its own trap frame with no kernel continuation, so the
// result has to be seated where the restore reloads r4 from. ctx->sp is the base of the
// {r4-r11, EXC_RETURN} block and the thread is not running, so this is a plain store to
// memory nothing else holds. r4 is the register the trap's own ABI answers in
// (arch_syscall_reg in switch.S), not the AAPCS r0.
void arch_ctx_set_syscall_result(struct arch_context* ctx, uint32_t result)
{
    reinterpret_cast<uint32_t*>(ctx->sp)[0] = result;
}
#endif

// The whole seam on this backend. The fabricated frame carries EXC_RETURN 0xFFFFFFFD
// (thread mode, PSP, NON-FP frame), so the rebuild also RESETS the frame format: a
// thread that had an extended FP frame stacked resumes on a plain 8-word one, which is
// what the new EXC_RETURN says. That is only sound because every frame it had is
// discarded here.
void arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                       void* stack_base, size_t stack_size)
{
    arch_context_init(ctx, entry, nullptr, stack_base, stack_size, 1);
}

// --- Critical section: raise BASEPRI to the kernel lock threshold -----------
arch_irq_state_t arch_irq_save(void)
{
    uint32_t prev;
    __asm volatile("mrs %0, basepri" : "=r"(prev));
    // Nested-lock fast path: if BASEPRI already masks at least as strongly as the
    // lock, the section is already in effect: no BASEPRI change and thus no barrier
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
    // (ARMv7-M ARM, "Barriers": a BASEPRI write needs DSB+ISB to take effect).
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
// dedicated peripheral timer). There is deliberately no DWT fallback TU here: the
// DWT is debug-domain (gated by DEMCR.TRCENA, lockable on Cortex-M7, absent under
// QEMU) and every chip that ever relied on the old fallback hit a broken clock, so a
// board that forgets to provide one must fail LOUD at link time, not hang on its
// first sleep. The one-shot SysTick timer is core-generic (arch_arm_common).

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
    // Latch-and-coalesce: PRESERVE any latched NVIC pending across enable. A raise
    // that arrived while the line was masked fires through the normal ISR path the
    // instant ISER is set. Drain a preceding device-flag clear (the W1C may still sit
    // in the write buffer, and exception entry does not order device writes) so a
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
    // Drain any pending device write, then drop the latched NVIC pending (ICPR).
    __asm volatile("dsb" ::: "memory");
    reg32(NVIC_ICPR0 + (l >> 5) * 4) = 1u << (l & 31);
}

// arch_shutdown has a fallback TU in arch/arm/common (mask + halt); a chip that
// exits through a debug channel (QEMU semihosting) strong-overrides it there.

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

// NOT ctx.resting_npriv: that says the thread is a user thread, while syscall dispatch
// runs PRIVILEGED in thread mode on the thread's own stack (switch.S svc_trampoline), so
// a fault there is a kernel bug. Exception entry does not modify CONTROL, so reading it
// here gives the privilege at fault time. A non-zero stacked IPSR means the fault
// escalated from inside another handler: also a kernel bug.
bool arch_fault_is_user_thread(void* frame)
{
    uint32_t control;
    __asm volatile("mrs %0, control" : "=r"(control));
    if ((control & 1u) == 0u)
    {
        return false;
    }
    // MSTKERR/MUNSTKERR (CFSR bits 4/3) and STKERR/UNSTKERR (bits 12/11) mean the
    // hardware aborted mid-stacking, so `frame` addresses memory the frame was never
    // written to and f[7] below would be whatever RAM already held. A stack overflow
    // arrives exactly this way; declining sends it to the panic dump.
    if (kickos::arm::reg32(0xE000ED28) & 0x1818u)
    {
        return false;
    }
    // Neither test subsumes the other: the CFSR bits catch a stacking abort whose SP was
    // still in range, this catches a frame written in full at a wild SP, which sets no
    // CFSR bit at all.
    if (not kickos_fault_frame_trusted(frame, 32))
    {
        return false;
    }
    uint32_t const* const f = static_cast<uint32_t const*>(frame);
    return (f[7] & 0x1FFu) == 0u; // stacked xPSR IPSR field
}

// Entered by the exception return with r0 = the SP the stub must run on. Naked, and the
// SP move is the first instruction: anything the compiler put before it would run on the
// stack this exists to leave. Thread mode does not change SPSEL, so this writes the PSP
// the switcher gave the thread, which is where svc_trampoline runs privileged too.
__attribute__((naked, noreturn)) void kickos_armv7m_fault_stack_reset(void)
{
    __asm volatile("mov sp, r0\n\t"
                   "b   kickos_thread_fault_exit");
}

void arch_fault_redirect_to_exit(void* frame)
{
    uint32_t const cfsr = kickos::arm::reg32(0xE000ED28);
    uint32_t const hfsr = kickos::arm::reg32(0xE000ED2C);
    uintptr_t addr = 0;
    int addr_valid = 0;
    // MMFAR/BFAR hold a stale address unless the matching VALID bit is set
    // (MMARVALID = CFSR bit 7, BFARVALID = bit 15).
    if (cfsr & (1u << 7))
    {
        addr = kickos::arm::reg32(0xE000ED34);
        addr_valid = 1;
    }
    else if (cfsr & (1u << 15))
    {
        addr = kickos::arm::reg32(0xE000ED38);
        addr_valid = 1;
    }
    uint32_t* const f = static_cast<uint32_t*>(frame);
    kickos_fault_record("CFSR", cfsr, f[6], addr, addr_valid);
    // Both are write-1-to-clear and sticky, and this fault is not the last one: a bit
    // left set would mislabel the NEXT thread's fault with this one's status.
    kickos::arm::reg32(0xE000ED28) = cfsr;
    kickos::arm::reg32(0xE000ED2C) = hfsr;

    // The stub runs at the top of this thread's stack, not at the depth the fault reached.
    // The frame is NOT relocated to get there: with lazy FP stacking the EXC_RETURN still
    // in the handler's LR decides whether the CPU unstacks a basic 8-word or an extended
    // 26-word frame (ARMv7-M ARM B1.5.7), and a frame moved to a place that EXC_RETURN
    // disagrees with is popped at the wrong size out of the wrong memory. So the hardware
    // pops from where it stacked, and the reset shim's first instruction, reached only
    // after that pop, moves SP. r0 carries the new SP because it is the frame's own
    // first word and this thread is dying.
    //
    // The 8-byte alignment the AAPCS wants at a public interface is the mask below, and
    // the stack-realign bit 9 is left alone: it belongs to the pop, which still happens at
    // the original SP.
    uint32_t const top = static_cast<uint32_t>(kickos_fault_stack_top());
    if (top != 0)
    {
        f[0] = top & ~7u;
        f[6] = reinterpret_cast<uint32_t>(&kickos_armv7m_fault_stack_reset) & ~1u;
    }
    else
    {
        f[6] = reinterpret_cast<uint32_t>(&kickos_thread_fault_exit) & ~1u; // drop the Thumb bit
    }
    // Keep T (bit 24) and the stack-realign bit 9, clear IT/ICI (bits 26:25 and 15:10):
    // a fault inside an IT block would otherwise resume with stale condition state and
    // conditionally skip the stub's first instructions.
    f[7] = (f[7] & ~((3u << 25) | (0x3Fu << 10))) | (1u << 24);
    // Exception return does not restore CONTROL, so clearing nPRIV here is what makes
    // the stub privileged. SPSEL is the bit handler mode ignores; nPRIV is not.
    uint32_t control;
    __asm volatile("mrs %0, control" : "=r"(control));
    __asm volatile("msr control, %0" ::"r"(control & ~1u));
    __asm volatile("isb" ::: "memory");
}

// C side of the fault handler: `frame` points at the hardware-stacked exception
// frame {r0,r1,r2,r3,r12,lr,pc,xPSR}; `exc_return` is the EXC_RETURN in LR (bit 2
// selects the pre-fault stack). Dump it plus the fault-status registers, then hand
// off to the shared terminal (blink on real HW, exit on host/QEMU).
void kickos_armv7m_fault_report(uint32_t* frame, uint32_t exc_return)
{
    // HardFault_Handler reaches here by a plain `b`, so this function's own return IS the
    // exception return. Nothing may print above: kpanic_enter's console reclaim is
    // permanent and this fault is survivable.
    if (kickos_fault_kill_thread(frame))
    {
        return;
    }
    kpanic_enter(); // mask IRQs + force the sync path + flush queued bytes, in order
#if KICKOS_PANIC_DUMP
    uint32_t cfsr = kickos::arm::reg32(0xE000ED28);
    uint32_t hfsr = kickos::arm::reg32(0xE000ED2C);
    char const* stk = "MSP";
    if (exc_return & 0x4u)
    {
        stk = "PSP";
    }
    // Label from the CFSR byte that is set: MMFSR is CFSR[7:0], BFSR is CFSR[15:8].
    char const* label = "HARD FAULT";
    if (cfsr & 0xFFu)
    {
        label = "MPU FAULT";
    }
    else if (cfsr & 0xFF00u)
    {
        label = "BUS FAULT";
    }
    ::kickos::kprintf("\n=== %s ===\n", label);
    ::kickos::kprintf(KDIAG_F_ARM_REGS1, frame[6], frame[5], frame[7], stk);
    ::kickos::kprintf(KDIAG_F_ARM_REGS2, frame[0], frame[1], frame[2], frame[3], frame[4]);
    ::kickos::kprintf(KDIAG_F_ARM_CFSR, cfsr, hfsr);
    if (cfsr & (1u << 10)) // BFSR IMPRECISERR: the stacked PC is past the faulting store
    {
        ::kickos::kprintf(KDIAG_F_ARM_IMPRECISE);
    }
    // MMFAR/BFAR only hold a valid address when the matching CFSR VALID bit is set
    // (MMARVALID = bit 7, BFARVALID = bit 15); otherwise their contents are stale.
    if (cfsr & (1u << 7))
    {
        ::kickos::kprintf(KDIAG_F_ARM_MMFAR, kickos::arm::reg32(0xE000ED34));
    }
    if (cfsr & (1u << 15))
    {
        ::kickos::kprintf(KDIAG_F_ARM_BFAR, kickos::arm::reg32(0xE000ED38));
    }
    arch_fault_report_extra(); // chip hook: e.g. K64F SYSMPU error capture
#else
    (void)frame;
    (void)exc_return;
    ::kickos::kprintf("\n=== HARD FAULT ===\n");
#endif
    kfault_terminate();
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
