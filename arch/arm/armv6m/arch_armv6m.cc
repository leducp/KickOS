// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ARMv6-M (Cortex-M0/M0+) arch backend: the Cortex-M0 core-generic half of the
// arch.h seam. Context switch + syscall trap are in switch.S. Versus armv7m:
// the critical section is PRIMASK (mask ALL configurable interrupts; v6-M has
// no BASEPRI), and there is no DWT cycle counter, so arch_clock_now is supplied
// by the chip (like arch_console_write), not here.

#include <kickos/arch/arch.h>
#include <kickos/diag.h>

#include "regs.h"
#include <kickos/trace/record.h> // ArchId: pin this build's trace-arch id to this backend

#include <stddef.h> // offsetof

// The trace-arch id (CMake ladder / this chip's caps.cmake) must equal the ArchId
// for the arch this backend implements, or a SESSION record mislabels the trace.
// A wrong caps.cmake value breaks the build here instead of drifting silently.
static_assert(KICKOS_TRACE_ARCH == kickos::trace::ARCH_ARMV6M,
              "KICKOS_TRACE_ARCH does not match ArchId::ARCH_ARMV6M for armv6m");

// Fault reporting (the shared HardFault handler below): the reporter calls
// kpanic_enter first, which masks IRQs, forces the synchronous polled writer, and
// flushes the ring, so the dump is safe from fault context whether or not the
// chip armed the buffered console (rp2040 does, nrf51 does not). kfault_terminate
// is the shared panic/fault dead-end (kernel.h; nrf51 overrides it to exit under
// QEMU, rp2040 uses the blink fallback).
namespace kickos
{
    void kprintf(char const* fmt, ...);
}
extern "C" void kpanic_enter(void);
extern "C" void kfault_terminate(void) __attribute__((noreturn));

// 0 keeps only the one-line fault marker; set it in the board defconfig or with
// cmake -DKICKOS_PANIC_DUMP=0.
#ifndef KICKOS_PANIC_DUMP
#define KICKOS_PANIC_DUMP 1
#endif

// Deferred MPU commit is shared; see arch/arm/common.

static_assert(offsetof(struct arch_context, sp) == 0, "switch.S expects ctx.sp @0");
static_assert(offsetof(struct arch_context, npriv) == 4, "switch.S expects ctx.npriv @4");
static_assert(offsetof(struct arch_context, resting_npriv) == 8,
              "switch.S expects ctx.resting_npriv @8");
// The PSP bounds guard reads these two as plain displacements, so a reorder would have it
// compare a PSP against the wrong words and pass one with no room below it. Both offsets are
// UNCONDITIONAL: the telemetry field is last so no build posture moves them.
static_assert(offsetof(struct arch_context, stack_lo) == KICKOS_ARMV6M_CTX_OFF_STACK_LO,
              "switch.S reads ctx.stack_lo at F_CTX_STACK_LO");
static_assert(offsetof(struct arch_context, stack_hi) == KICKOS_ARMV6M_CTX_OFF_STACK_HI,
              "switch.S reads ctx.stack_hi at F_CTX_STACK_HI");
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
static_assert(offsetof(struct arch_context, trace_tid) == KICKOS_ARMV6M_CTX_OFF_TRACE_TID,
              "switch.S telemetry hook expects ctx.trace_tid at F_CTX_TRACE_TID");
#endif

namespace
{
    // The {r4-r11} block the two software pushes write below the live PSP, and the block
    // arch_context_init fabricates for a first switch-in. Drives both the fabrication loop
    // and the assertion that F_TRAP_FRAME prices exactly this register list: gas cannot
    // count the registers in an stmia.
    constexpr size_t CALLEE_BLOCK_WORDS = 8;
}
static_assert(CALLEE_BLOCK_WORDS * sizeof(uint32_t) == KICKOS_ARMV6M_TRAP_FRAME,
              "PSP_GUARD's F_TRAP_FRAME prices {r4-r11}: eight words");

namespace
{
    using namespace kickos::arm;    // reg32, NVIC_ISER0 (shared core regs)
    using namespace kickos::armv6m; // SHPR2/3, PRIO_* (arch-specific)
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
    for (size_t i = 0; i < CALLEE_BLOCK_WORDS; i++)
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

    // PendSV and the SVC fastpath check the live PSP against these before either pushes
    // {r4-r11} through it. `top` is the 8-byte-aligned high edge the first frame sits
    // below, so a running thread's PSP stays in [stack_lo, stack_hi).
    ctx->stack_lo = reinterpret_cast<uint32_t>(stack_base);
    ctx->stack_hi = static_cast<uint32_t>(top);
}

// The whole seam on this backend: the fabricated first frame already lands at the stack
#if defined(KICKOS_ARCH_HAS_IPC_FASTPATH) && KICKOS_ARCH_HAS_IPC_FASTPATH
// The fastpath parks a caller on its own trap frame with no kernel continuation, so the
// result has to be seated where the restore reloads r4 from. ctx->sp is the base of the
// {r4-r11} block and the thread is not running, so this is a plain store to memory
// nothing else holds. r4 is the register the trap's own ABI answers in (arch_syscall_reg
// in switch.S), not the AAPCS r0.
void arch_ctx_set_syscall_result(struct arch_context* ctx, uint32_t result)
{
    reinterpret_cast<uint32_t*>(ctx->sp)[0] = result;
}
#endif

// top with CONTROL.nPRIV expressing privilege, so a rebuild is that same fabrication.
void arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                       void* stack_base, size_t stack_size)
{
    arch_context_init(ctx, entry, nullptr, stack_base, stack_size, 1);
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

// --- Interrupt controller (NVIC). No priority band: the crit section is
// PRIMASK (masks all), so device-IRQ priorities don't gate it as on v7-M. The
// mask/inject halves are core-generic and live in arch_arm_common.cc; only the
// v6-M "enable, no priority-band program" unmask is here. ---------------------
void arch_irq_unmask(int line)
{
    if (line < 0)
    {
        return;
    }
    unsigned l = static_cast<unsigned>(line);
    // Latch-and-coalesce: PRESERVE any latched NVIC pending across enable. A raise
    // taken while the line was masked fires through the normal ISR path the instant
    // ISER is set (see the v7-M note). Drain a preceding device-flag clear (dsb) so
    // a genuinely deasserted level source does not re-latch.
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
    __asm volatile("dsb" ::: "memory");
    reg32(NVIC_ICPR0 + (l >> 5) * 4) = 1u << (l & 31);
}

// --- Kernel-facing ISR entries ----------------------------------------------
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

// --- Fault reporting: the shared HardFault handler. HardFault is the ONLY fault
// exception on v6-M (no MemManage/BusFault/UsageFault, and no CFSR/HFSR/MMFAR/BFAR
// fault-status registers), so the dump is the stacked frame only. Replaces the
// per-chip stubs that discarded everything. ----------------------------------------
void kickos_armv6m_fault_report(uint32_t* frame, uint32_t exc_return)
{
    // The naked handler reaches here by `bx`, so this function's own return IS the
    // exception return. Nothing may print above: kpanic_enter's console reclaim is
    // permanent and this fault is survivable.
    if (kickos_fault_kill_thread(frame))
    {
        return;
    }
    kpanic_enter(); // mask IRQs + force the sync path + flush queued bytes, in order
    ::kickos::kprintf("\n=== HARD FAULT ===\n");
#if KICKOS_PANIC_DUMP
    char const* stk = "MSP";
    if (exc_return & 0x4u)
    {
        stk = "PSP";
    }
    ::kickos::kprintf(KDIAG_F_ARM_REGS1, frame[6], frame[5], frame[7], stk);
    ::kickos::kprintf(KDIAG_F_ARM_REGS2, frame[0], frame[1], frame[2], frame[3], frame[4]);
#else
    (void)frame;
    (void)exc_return;
#endif
    kfault_terminate();
}

// Naked entry: pick the stacked frame (MSP vs PSP per EXC_RETURN bit 2) and pass it,
// with EXC_RETURN, to the C reporter. Naked so no prologue perturbs the SP before we
// read it. v6-M has no IT block, so the stack select is branch-based (movs/tst/beq/
// mrs); the v7-M ite/mrseq form is illegal here.
__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        "mov  r1, lr          \n"
        "movs r0, #4          \n"
        "tst  r0, r1          \n"
        "beq  1f              \n"
        "mrs  r0, psp         \n"
        "b    2f              \n"
        "1:                   \n"
        "mrs  r0, msp         \n"
        "2:                   \n"
        // ldr+bx, not b: the Thumb-1 unconditional branch is +-2 KB, and with
        // --gc-sections the reporter can land farther than that from this handler.
        "ldr  r2, =kickos_armv6m_fault_report \n"
        "bx   r2              \n");
}

// Called from switch.S when the running thread's live PSP has no room BELOW it for the
// {r4-r11} block the push is about to write there. Exception entry stacks the hardware
// frame ABOVE the PSP under the pre-exception privilege, so the MPU refuses only that
// half: a PSP aimed a few words above a stack's base passes entry and lands the software
// block in the NEIGHBOURING thread's own granted memory, where r10 and r11 overwrite that
// thread's stacked PC and xPSR.
//
// Runs in handler mode on the MSP. Contains the system rather than the write: there is no
// frame to trust and no PSP a resume could be handed.
void kickos_armv6m_bad_psp(uint32_t psp, uint32_t need, uint32_t lo, uint32_t hi)
{
    kpanic_enter();
#if KICKOS_PANIC_DUMP
    // WHICH bound refused, re-derived from the values: the three legs share one guard.
    char const* why = "no room below";
    if (psp < lo)
    {
        why = "under stack_lo";
    }
    else if (psp >= hi)
    {
        why = "at or above stack_hi";
    }
    // WHICH guarded push refused, read from ICSR.VECTACTIVE rather than passed in: both
    // reach here through one guard, and nothing in the arguments separates them.
    uint32_t const vect = reg32(0xE000ED04) & 0x1FFu;
    char const* site = "handler";
    if (vect == 11u)
    {
        site = "SVCall";
    }
    else if (vect == 14u)
    {
        site = "PendSV";
    }
    ::kickos::kprintf("\n=== ARMV6M EXCEPTION (wild PSP: %s) ===\n", why);
    ::kickos::kprintf("  in %s PSP=0x%x need=%u stack=[0x%x,0x%x)\n", site,
                      static_cast<unsigned>(psp), static_cast<unsigned>(need),
                      static_cast<unsigned>(lo), static_cast<unsigned>(hi));
#else
    (void)psp;
    (void)need;
    (void)lo;
    (void)hi;
    ::kickos::kprintf("\n=== ARMV6M EXCEPTION (wild PSP) ===\n");
#endif
    kfault_terminate();
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
