// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ARMv7-M arch backend: the arch.h seam parts this profile does not share with ARMv6-M.

#include <kickos/arch/arch.h>
#include "ctx_redirect.h"
#include "../common/fault_resume.h"
#include <kickos/diag.h>
#include <kickos/units.h>

#include "regs.h"
#include <kickos/arch/armv7m_fault_frame.h>
#include <kickos/arch/armv7m_trap_stack.h>
#include <kickos/trace/record.h>

#include <stddef.h> // offsetof
#include <stdint.h>

// A mismatch mislabels every SESSION record.
static_assert(KICKOS_TRACE_ARCH == kickos::trace::ARCH_ARMV7M,
              "KICKOS_TRACE_ARCH does not match ArchId::ARCH_ARMV7M for armv7m");

// switch.S hard-codes these offsets.
static_assert(offsetof(struct arch_context, sp) == 0, "switch.S expects ctx.sp @0");
static_assert(offsetof(struct arch_context, npriv) == 4, "switch.S expects ctx.npriv @4");
static_assert(offsetof(struct arch_context, resting_npriv) == 8,
              "switch.S expects ctx.resting_npriv @8");
// The PSP bounds guard reads these as plain displacements. The telemetry field is last so no
// build posture moves them.
static_assert(offsetof(struct arch_context, stack_lo) == KICKOS_ARMV7M_CTX_OFF_STACK_LO,
              "switch.S reads ctx.stack_lo at KICKOS_ARMV7M_CTX_OFF_STACK_LO");
static_assert(offsetof(struct arch_context, stack_hi) == KICKOS_ARMV7M_CTX_OFF_STACK_HI,
              "switch.S reads ctx.stack_hi at KICKOS_ARMV7M_CTX_OFF_STACK_HI");
// Unconditional even at KICKOS_KERNEL_STACKS 0: the field is in the struct on every build.
static_assert(offsetof(struct arch_context, kernel_sp) == KICKOS_ARMV7M_CTX_OFF_KERNEL_SP,
              "svc_trampoline and PendSV_Handler load ctx.kernel_sp at F_CTX_KERNEL_SP");

// gas cannot count the registers in an stmdb.
static_assert(KICKOS_ARMV7M_TRAP_FRAME == 9u * sizeof(uint32_t),
              "PSP_GUARD prices {r4-r11, EXC_RETURN}: nine words");
static_assert(KICKOS_ARMV7M_TRAP_FRAME_FP == 16u * sizeof(uint32_t),
              "the FP term prices {s16-s31}: sixteen words");
static_assert(KICKOS_ARMV7M_TRAP_FRAME_MAX
                  == KICKOS_ARMV7M_TRAP_FRAME + KICKOS_ARMV7M_TRAP_FRAME_FP,
              "FRAME_MAX is the worst-case push and is what the red-zone gate scrapes");
// One hoisted guard charges both arms the same figure.
static_assert(KICKOS_ARMV7M_TRAP_NEST_SVC >= KICKOS_ARMV7M_TRAP_FRAME_MAX,
              "the SVC site must cover the fastpath arm's FP-live push, which it guards too");
// The SVC window, derived in armv7m_trap_stack.h.
static_assert(KICKOS_ARMV7M_TRAP_NEST_SVC
                  == 8 + 104 + KICKOS_ARMV7M_TRAP_FRAME_MAX - 32,
              "the SVC window is the unstack credit, the trampoline's prologue, a "
              "preempting hardware frame and the PendSV block that tail-chains below it");
#if !KICKOS_KERNEL_STACKS
// The gate scrapes both figures as immediates and cannot catch a drift between them.
static_assert(KICKOS_ARMV7M_TRAP_NEST_SVC_DISPATCH == KICKOS_ARMV7M_TRAP_NEST_SVC + 4,
              "the unconverted SVC window is the converted one plus the STKALIGN pad a "
              "preempting entry spends below a chain of compiler frames");
static_assert(KICKOS_MAX_SPAWN_GRANTS <= 9,
              "KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVC holds a spawn staging 9 grants on the "
              "caller's stack, and 10 measure past it: re-measure with "
              "tests/static/check_trap_redzone.sh and raise the figure first");
#endif
#if KICKOS_KERNEL_STACKS
static_assert(KICKOS_ARMV7M_TRAP_NEST_SVCK
                  == 16 + 4 + 104 + KICKOS_ARMV7M_TRAP_FRAME_MAX,
              "the SVCK structural half is the continuation header, the STKALIGN pad, a "
              "preempting hardware frame and the PendSV block that tail-chains below it");
// SVCK counts the panic tail SVC excludes, so it cannot be the smaller; the gate scrapes
// both as immediates.
static_assert(KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVCK >= KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVC,
              "the tail-counted dispatch depth is below the tail-excluded one");
#if KICKOS_TELEMETRY or KICKOS_BENCH
static_assert(KICKOS_MAX_SPAWN_GRANTS <= 9,
              "KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVCK was measured at a spawn staging 9 grants on "
              "the kernel block: re-measure with tests/static/check_trap_redzone.sh first");
#else
static_assert(KICKOS_MAX_SPAWN_GRANTS <= 12,
              "KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVCK was measured at a spawn staging 12 grants on "
              "the kernel block: re-measure with tests/static/check_trap_redzone.sh first");
#endif
#endif
// Handler mode forces SP_main, so everything PendSV_Handler calls runs on the MSP.
static_assert(KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_PENDSV == 0,
              "a nonzero PendSV descent needs roots in tests/static/trap_redzone_roots.txt");
static_assert(KICKOS_ARMV7M_TRAP_NEED_SVC > KICKOS_ARMV7M_TRAP_NEED_PENDSV,
              "the SVC site must charge more than the switcher: it keeps running on the PSP");
// The floor must DOMINATE the worst-case red zone, or a thread spawned at the floor passes
// the spawn check and is refused by the guard on every syscall. The hardware spends the entry
// frame ABOVE the PSP before any handler runs, so a floor-sized empty stack offers the guard
// only KICKOS_MIN_STACK_SIZE minus that frame. The guard asks for NEED_SVC unconditionally,
// so the FP-live 104 binds, 72 bytes stricter than needed: the price of not branching on
// FPCA at the SVC site.
static_assert(KICKOS_MIN_STACK_SIZE
                  >= KICKOS_ARMV7M_TRAP_NEED_SVC + KICKOS_ARMV7M_TRAP_ENTRY_FRAME,
              "KICKOS_MIN_STACK_SIZE is below the armv7m syscall red zone plus the "
              "exception frame entry spends above it: raise the per-arch default in "
              "Kconfig, never the red zone, which is a measurement");

// The reporter's own array (kernel/init/console.cc); the frame term is the hardware frame a
// HardFault stacks there.
static_assert(KICKOS_ARMV7M_PANIC_FRAME == KICKOS_ARMV7M_TRAP_FRAME_MAX,
              "the panic frame term is the hardware frame, which switch.S cannot compute");
static_assert(KICKOS_PANIC_STACK_SIZE >= KICKOS_ARMV7M_PANIC_FRAME + KICKOS_ARMV7M_PANIC_DEPTH,
              "KICKOS_PANIC_STACK_SIZE is below what this arch's panic reporter descends");

static_assert(KICKOS_KERNEL_STACK_SIZE % 8 == 0,
              "KICKOS_KERNEL_STACK_SIZE must be a multiple of 8 on this arch, or a "
              "kernel stack's top does not land on the alignment every frame on it "
              "assumes");
#if KICKOS_KERNEL_STACKS
// PendSV_Handler loads the size with movw.
static_assert(KICKOS_KERNEL_STACK_SIZE <= 0xFFFF,
              "PendSV_Handler's kernel-block leg loads KICKOS_KERNEL_STACK_SIZE with movw");
// The lowest word of the block is the overflow canary (kernel/thread/thread.cc), so the
// requirement must fit ABOVE it. Both sides resolve per KICKOS_TELEMETRY; a board raising one
// without the other fails here.
//
// EXITK is the fault and slay stubs on the kernel block. The kstacks=0 fallback below must
// stay outside this #if: it is false on the boards that fallback describes.
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t)
                  >= KICKOS_ARMV7M_TRAP_NEST_EXIT + KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_EXITK,
              "the kernel block cannot hold the relocated death path plus its canary word");
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t) >= KICKOS_ARMV7M_TRAP_NEED_SVCK,
              "KICKOS_KERNEL_STACK_SIZE is below the armv7m syscall kernel-stack "
              "requirement plus its canary word: raise the per-arch default in Kconfig, "
              "never the depth, which is a measurement");
#endif
#if !KICKOS_KERNEL_STACKS
static_assert(KICKOS_MIN_STACK_SIZE
                  >= KICKOS_ARMV7M_TRAP_NEST_EXIT + KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_EXIT,
              "the spawn floor cannot hold the death path where no kernel block is seated");
#endif
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
static_assert(offsetof(struct arch_context, trace_tid) == KICKOS_ARMV7M_CTX_OFF_TRACE_TID,
              "switch.S telemetry hook reads ctx.trace_tid at F_CTX_TRACE_TID");
#endif
static_assert(kickos::armv7m::PRIO_LOCK_BASEPRI == 0x20,
              "SVC_Handler's fastpath raises this level as a literal");

namespace
{
    using namespace kickos::arm;
    using namespace kickos::armv7m;
}

extern "C"
{
    // An unprivileged thread whose entry returns cannot run kickos_thread_return: with
    // nPRIV=1 the BASEPRI write in IrqLock is a no-op and the SCS write in arch_switch
    // BusFaults. This one traps out via the exit syscall.
    void kickos_user_thread_return(void);

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

extern "C"
{

// --- Context init: fabricate a first-switch-in frame -----------------------
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

    *(--sp) = 0x01000000u;                                    // xPSR (Thumb bit)
    *(--sp) = reinterpret_cast<uint32_t>(entry) & ~1u;        // PC = entry
    *(--sp) = ret;                                            // LR: entry returns here
    *(--sp) = 0;                                              // r12
    *(--sp) = 0;                                              // r3
    *(--sp) = 0;                                              // r2
    *(--sp) = 0;                                              // r1
    *(--sp) = reinterpret_cast<uint32_t>(arg);               // r0 = arg

    // PendSV-saved block, popped by ldmia with r4 lowest and EXC_RETURN highest.
    *(--sp) = 0xFFFFFFFDu; // EXC_RETURN: thread mode, PSP, non-FP frame
    for (int i = 0; i < 8; i++)
    {
        *(--sp) = 0; // r11..r4
    }

    ctx->sp = reinterpret_cast<uint32_t>(sp);
    // CONTROL.nPRIV: 0 = privileged, 1 = unprivileged.
    uint32_t npriv = 1;
    if (privileged)
    {
        npriv = 0;
    }
    ctx->npriv = npriv;
    ctx->resting_npriv = npriv;

    // The PSP guard in PendSV and SVC_Handler checks against these; `top` is the aligned
    // high edge.
    ctx->stack_lo = reinterpret_cast<uint32_t>(stack_base);
    ctx->stack_hi = static_cast<uint32_t>(top);

    // ctx->kernel_sp IS UNTOUCHED: thread_create seats it before this call, and zero is its
    // no-block marker.
}

// The fabricated frame carries EXC_RETURN 0xFFFFFFFD (thread mode, PSP, NON-FP frame), so
// the rebuild also RESETS the frame format: a thread that had an extended FP frame stacked
// resumes on a plain 8-word one. Sound only because every frame it held is discarded here.
void arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                       void* stack_base, size_t stack_size)
{
    arch_ctx_redirect_to_block(ctx, entry, stack_base, stack_size);
}

// --- Critical section: raise BASEPRI to the kernel lock threshold -----------
arch_irq_state_t arch_irq_save(void)
{
    uint32_t prev;
    __asm volatile("mrs %0, basepri" : "=r"(prev));
    // A lower BASEPRI masks more, 0 masks nothing. Already at least as strong as the lock:
    // skip the write; a weaker prev (a device band 0x30) still raises to the lock.
    if (prev != 0 and prev <= PRIO_LOCK_BASEPRI)
    {
        return prev;
    }
    __asm volatile("msr basepri, %0" ::"r"(PRIO_LOCK_BASEPRI) : "memory");
    // Raising BASEPRI is not self-synchronizing: an interrupt could otherwise be taken under
    // the OLD mask.
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
    return prev;
}

// --- Monotonic clock ---------------------------------------------------------
// arch_clock_now is the chip's, over a dedicated peripheral timer: the DWT is debug-domain
// (gated by DEMCR.TRCENA, lockable on Cortex-M7, absent under QEMU). A board omitting it fails
// at link time rather than hanging on its first sleep.

// --- Fault reporting ----------------------------------------------------------
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

// CONTROL.nPRIV and NOT ctx.resting_npriv: syscall dispatch runs PRIVILEGED in thread mode
// (switch.S svc_trampoline) on behalf of a user thread, so a fault there is a kernel bug.
// Exception entry does not modify CONTROL, so reading it here gives the privilege at fault
// time. A non-zero stacked IPSR means the fault escalated from inside another handler.
//
// The frame is always on the USER stack under either entry design: the hardware stacks it at
// the PSP, and an accepted fault here is one taken with nPRIV set, which the dispatch never
// is. So the test below is the thread-stack one, not its kernel-block twin.
bool arch_fault_is_user_thread(void* frame)
{
    uint32_t control;
    __asm volatile("mrs %0, control" : "=r"(control));
    if ((control & 1u) == 0u)
    {
        return false;
    }
    if (not armv7m_fault_frame_readable(kickos::arm::reg32(SCB_CFSR)))
    {
        return false;
    }
    // Neither test subsumes the other: the CFSR bits catch a stacking abort whose SP was
    // still in range, this catches a frame written in full at a wild SP, which sets no
    // CFSR bit at all.
    if (not kickos_fault_frame_trusted(frame, ARMV7M_BASIC_FRAME_BYTES))
    {
        return false;
    }
    uint32_t const* const f = static_cast<uint32_t const*>(frame);
    return (f[7] & 0x1FFu) == 0u; // stacked xPSR IPSR field
}

// Entered by the exception return with r0 = the SP the stub must run on. The SP move must
// be the first instruction: anything the compiler put before it would run on the stack this
// exists to leave. Thread mode does not change SPSEL, so this writes the PSP.
__attribute__((naked, noreturn)) void kickos_armv7m_fault_stack_reset(void)
{
    __asm volatile("mov sp, r0\n\t"
                   "b   kickos_thread_fault_exit");
}

void arch_fault_redirect_to_exit(void* frame)
{
    uint32_t const cfsr = kickos::arm::reg32(SCB_CFSR);
    uint32_t const hfsr = kickos::arm::reg32(SCB_HFSR);
    uintptr_t addr = 0;
    int addr_valid = 0;
    // Taken whatever the core latched: a chip latch left set labels the NEXT thread's fault.
    uintptr_t chip_addr = 0;
    bool const chip_latched = arch_fault_chip_addr(&chip_addr);
    // MMFAR/BFAR hold a stale address unless the matching VALID bit is set.
    if (cfsr & ARMV7M_CFSR_MMARVALID)
    {
        addr = kickos::arm::reg32(SCB_MMFAR);
        addr_valid = 1;
    }
    else if (cfsr & ARMV7M_CFSR_BFARVALID)
    {
        addr = kickos::arm::reg32(SCB_BFAR);
        addr_valid = 1;
    }
    else if (chip_latched and armv7m_chip_addr_explains(cfsr))
    {
        addr = chip_addr;
        addr_valid = 1;
    }
    uint32_t* const f = static_cast<uint32_t*>(frame);
    kickos_fault_record("CFSR", cfsr, f[6], addr, addr_valid);
    // Write-1-to-clear and sticky: a bit left set mislabels the NEXT thread's fault.
    kickos::arm::reg32(SCB_CFSR) = cfsr;
    kickos::arm::reg32(SCB_HFSR) = hfsr;

    kickos_arm_fault_resume_at(f, &kickos_armv7m_fault_stack_reset);
    // Keep T (bit 24) and the stack-realign bit 9, which belongs to the pop at the original
    // SP; clear IT/ICI (bits 26:25 and 15:10): a fault inside an IT block would otherwise
    // resume with stale condition state and conditionally skip the stub's first instructions.
    f[7] = (f[7] & ~((3u << 25) | (0x3Fu << 10))) | (1u << 24);
    kickos_arm_fault_resume_privileged();
}

// `frame` points at the hardware-stacked exception frame {r0,r1,r2,r3,r12,lr,pc,xPSR};
// `exc_return` is the EXC_RETURN in LR, whose bit 2 selects the pre-fault stack.
void kickos_armv7m_fault_report(uint32_t* frame, uint32_t exc_return)
{
    // HardFault_Handler reaches here by a plain `b`, so this function's own return IS the
    // exception return. Nothing may print above this: kpanic_enter's console reclaim is
    // permanent and this fault is survivable.
    bool const frame_read = armv7m_fault_frame_readable(kickos::arm::reg32(SCB_CFSR));
    if (kickos_fault_kill_thread(frame))
    {
        return;
    }
    kickos_fault_report_begin();
    kpanic_enter();
#if KICKOS_PANIC_DUMP
    uint32_t cfsr = kickos::arm::reg32(SCB_CFSR);
    uint32_t hfsr = kickos::arm::reg32(SCB_HFSR);
    char const* stk = "MSP";
    if (exc_return & 0x4u)
    {
        stk = "PSP";
    }
    char const* label = "HARD FAULT";
    if (cfsr & ARMV7M_CFSR_MMFSR)
    {
        label = "MPU FAULT";
    }
    else if (cfsr & ARMV7M_CFSR_BFSR)
    {
        label = "BUS FAULT";
    }
    else if (cfsr & ARMV7M_CFSR_UFSR)
    {
        label = "USAGE FAULT";
    }
    ::kickos::kprintf("\n=== %s ===\n", label);
    if (frame_read)
    {
        ::kickos::kprintf(KDIAG_F_ARM_REGS1, frame[6], frame[5], frame[7], stk);
        ::kickos::kprintf(KDIAG_F_ARM_REGS2, frame[0], frame[1], frame[2], frame[3], frame[4]);
    }
    else
    {
        ::kickos::kprintf(KDIAG_F_ARM_NOFRAME, reinterpret_cast<uint32_t>(frame), stk);
    }
    ::kickos::kprintf(KDIAG_F_ARM_CFSR, cfsr, hfsr);
    if (cfsr & ARMV7M_CFSR_IMPRECISERR) // the stacked PC is past the faulting store
    {
        ::kickos::kprintf(KDIAG_F_ARM_IMPRECISE);
    }
    // MMFAR/BFAR are stale unless the matching CFSR VALID bit is set.
    if (cfsr & ARMV7M_CFSR_MMARVALID)
    {
        ::kickos::kprintf(KDIAG_F_ARM_MMFAR, kickos::arm::reg32(SCB_MMFAR));
    }
    if (cfsr & ARMV7M_CFSR_BFARVALID)
    {
        ::kickos::kprintf(KDIAG_F_ARM_BFAR, kickos::arm::reg32(SCB_BFAR));
    }
    arch_fault_report_extra(); // chip hook: e.g. K64F SYSMPU error capture
#else
    (void)frame;
    (void)exc_return;
    (void)frame_read;
    ::kickos::kprintf("\n=== HARD FAULT ===\n");
#endif
    kfault_terminate();
}

// Naked so no prologue perturbs SP before it is read. The chip vector tables point the fault
// vectors all here.
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

// --- One-time core bring-up ------------------------------------------------
void kickos_armv7m_init(void)
{
    reg32(SCB_SHCSR) |= SHCSR_USGFAULTENA | SHCSR_BUSFAULTENA;

    // SHPR2[31:24] = SVCall (#11); SHPR3[23:16] = PendSV (#14), [31:24] = SysTick.
    reg32(SCB_SHPR2) = (reg32(SCB_SHPR2) & 0x00FFFFFFu) | (PRIO_SVCALL << 24);
    uint32_t shpr3 = reg32(SCB_SHPR3) & 0x0000FFFFu;
    shpr3 |= (PRIO_PENDSV << 16) | (PRIO_SYSTICK << 24);
    reg32(SCB_SHPR3) = shpr3;

    reg32(DCB_DEMCR) |= DEMCR_TRCENA;
    reg32(DWT_CYCCNT) = 0;
    reg32(DWT_CTRL) |= DWT_CTRL_CYCCNTENA;
}

}
