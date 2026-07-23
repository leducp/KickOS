// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ARMv6-M (Cortex-M0/M0+) arch backend: the Cortex-M0 core-generic half of the
// arch.h seam. Context switch + syscall trap are in switch.S. Versus armv7m:
// the critical section is PRIMASK (mask ALL configurable interrupts -- v6-M has
// no BASEPRI), and there is no DWT cycle counter, so arch_clock_now is supplied
// by the chip (like arch_console_write), not here.
//
// Runtime-validated on QEMU Cortex-M0 (microbit); see docs/reference/porting.md.

#include <kickos/arch/arch.h>

#include "regs.h"

#include <stddef.h> // offsetof

// Fault reporting (the shared HardFault handler below): the reporter calls
// kpanic_enter first, which masks IRQs, forces the synchronous polled writer, and
// flushes the ring -- so the dump is safe from fault context whether or not the
// chip armed the buffered console (rp2040 does, nrf51 does not). kfault_terminate
// is the shared panic/fault dead-end (kernel.h; nrf51 overrides it to exit under
// QEMU, rp2040 uses the weak blink).
namespace kickos
{
    void kprintf(char const* fmt, ...);
}
extern "C" void kpanic_enter(void);
extern "C" void kfault_terminate(void) __attribute__((noreturn));

// Verbose CPU-context dump. Default on; -DKICKOS_PANIC_DUMP=0 keeps only the
// one-line fault marker. Same knob and default on every arch reporter.
#ifndef KICKOS_PANIC_DUMP
#define KICKOS_PANIC_DUMP 1
#endif

// --- Deferred MPU commit (armv6m fix for the eager-apply / deferred-switch desync) --
// On armv6m the context switch is a PENDED PendSV: switch_to() runs to completion on
// the OUTGOING thread and the physical register/PSP swap only happens later, inside
// PendSV. The shared arch_mpu_apply reprograms the MPU immediately, so in that window
// the outgoing thread executes with the INCOMING thread's region set and faults on its
// own stack (selftest test 14 mutex_chain HardFault: cur=chA/MPU=chA while chC is the
// physical thread in mtx_spin). Fix: arch_mpu_apply here only STASHES the requested
// region set; kickos_armv6m_mpu_commit programs the hardware, and PendSV (switch.S)
// calls it AFTER the physical swap -- so the MPU is reprogrammed atomically with the
// context switch. v7-M is untouched (keeps the eager weak arch_mpu_apply).
#if KICKOS_HAVE_MPU
extern "C" void kickos_arm_mpu_program(struct arch_mpu_region const* regions, size_t n);

namespace
{
    // PMSA hardware caps descriptors at 8; stash a private copy (not a pointer) so the
    // commit never chases a TCB whose region set changed after the stash.
    constexpr size_t MAX_PEND_REGIONS = 8;
    arch_mpu_region g_pend_regions[MAX_PEND_REGIONS];
    size_t g_pend_count = 0;
}

// STRONG override of the weak shared arch_mpu_apply: record only, no hardware write.
extern "C" void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n)
{
    if (n > MAX_PEND_REGIONS)
    {
        n = MAX_PEND_REGIONS;
    }
    for (size_t i = 0; i < n; i++)
    {
        g_pend_regions[i] = regions[i];
    }
    g_pend_count = n;
}

// Commit the stashed set to the MPU hardware. Called from PendSV after the physical
// context swap. cpsid brackets the disable/reprogram/re-enable so a device IRQ (prio
// 0, above PendSV) cannot observe a half-programmed / disabled MPU.
extern "C" void kickos_armv6m_mpu_commit(void)
{
    uint32_t primask;
    __asm volatile("mrs %0, primask" : "=r"(primask));
    __asm volatile("cpsid i" ::: "memory");
    kickos_arm_mpu_program(g_pend_regions, g_pend_count);
    __asm volatile("msr primask, %0" ::"r"(primask) : "memory");
}
#else
// No MPU on this v6-M board (nrf51/microbit): PendSV still calls the commit symbol
// unconditionally, so provide an empty one.
extern "C" void kickos_armv6m_mpu_commit(void) {}
#endif

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
    // Latch-and-coalesce: PRESERVE any latched NVIC pending across enable -- a raise
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
    kpanic_enter(); // mask IRQs + force the sync path + flush queued bytes, in order
    ::kickos::kprintf("\n=== HARD FAULT ===\n");
#if KICKOS_PANIC_DUMP
    char const* stk = "MSP";
    if (exc_return & 0x4u)
    {
        stk = "PSP";
    }
    ::kickos::kprintf("  PC=0x%x LR=0x%x xPSR=0x%x (%s)\n",
                      frame[6], frame[5], frame[7], stk);
    ::kickos::kprintf("  R0=0x%x R1=0x%x R2=0x%x R3=0x%x R12=0x%x\n",
                      frame[0], frame[1], frame[2], frame[3], frame[4]);
#else
    (void)frame;
    (void)exc_return;
#endif
    kfault_terminate();
}

// Naked entry: pick the stacked frame (MSP vs PSP per EXC_RETURN bit 2) and pass it,
// with EXC_RETURN, to the C reporter. Naked so no prologue perturbs the SP before we
// read it. v6-M has no IT block, so the stack select is branch-based (movs/tst/beq/
// mrs) -- the v7-M ite/mrseq form is illegal here.
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
