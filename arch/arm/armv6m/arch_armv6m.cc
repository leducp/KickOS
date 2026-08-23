// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ARMv6-M (Cortex-M0/M0+) arch backend: the Cortex-M0 core-generic half of the
// arch.h seam. Context switch + syscall trap are in switch.S. The critical section
// is PRIMASK, which masks every configurable interrupt, and arch_clock_now is a chip
// contract here, as arch_console_write is, because the cycle source on this core is
// per-chip.

#include <kickos/arch/arch.h>
#include <kickos/arch/armv6m_trap_stack.h> // the figures switch.S's PSP guard enforces
#include <kickos/diag.h>

#include "regs.h"
#include <kickos/trace/record.h> // ArchId: pin this build's trace-arch id to this backend

#include <stddef.h> // offsetof

// The trace-arch id (CMake ladder / this chip's caps.cmake) must equal the ArchId for
// the arch this backend implements, or a SESSION record mislabels the trace.
static_assert(KICKOS_TRACE_ARCH == kickos::trace::ARCH_ARMV6M,
              "KICKOS_TRACE_ARCH does not match ArchId::ARCH_ARMV6M for armv6m");

// Fault reporting (the shared HardFault handler below): the reporter calls kpanic_enter
// first, which masks IRQs, forces the synchronous polled writer and flushes the ring, so
// the dump is safe from fault context on a chip with a buffered console as well as one
// without. kfault_terminate is the shared panic/fault dead-end (kernel.h).
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

// Deferred MPU commit lives in arch/arm/common.

static_assert(offsetof(struct arch_context, sp) == 0, "switch.S expects ctx.sp @0");
static_assert(offsetof(struct arch_context, npriv) == 4, "switch.S expects ctx.npriv @4");
static_assert(offsetof(struct arch_context, resting_npriv) == 8,
              "switch.S expects ctx.resting_npriv @8");
// The PSP bounds guard reads these two as plain displacements, so a reorder would have it
// compare a PSP against the wrong words and pass one with no room below it. The telemetry
// field is last, which keeps both offsets the same in every build posture.
static_assert(offsetof(struct arch_context, stack_lo) == KICKOS_ARMV6M_CTX_OFF_STACK_LO,
              "switch.S reads ctx.stack_lo at F_CTX_STACK_LO");
static_assert(offsetof(struct arch_context, stack_hi) == KICKOS_ARMV6M_CTX_OFF_STACK_HI,
              "switch.S reads ctx.stack_hi at F_CTX_STACK_HI");
static_assert(offsetof(struct arch_context, kernel_sp) == KICKOS_ARMV6M_CTX_OFF_KERNEL_SP,
              "svc_trampoline and PendSV_Handler load ctx.kernel_sp at F_CTX_KERNEL_SP");
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
static_assert(offsetof(struct arch_context, trace_tid) == KICKOS_ARMV6M_CTX_OFF_TRACE_TID,
              "switch.S telemetry hook expects ctx.trace_tid at F_CTX_TRACE_TID");
#endif

namespace
{
    // The {r4-r11} block the two software pushes write below the live PSP, and the block
    // arch_context_init fabricates for a first switch-in. It drives the fabrication loop
    // and the assertion below, which prices F_TRAP_FRAME against the register list
    // because gas cannot count the registers in an stmia.
    constexpr size_t CALLEE_BLOCK_WORDS = 8;
}
static_assert(CALLEE_BLOCK_WORDS * sizeof(uint32_t) == KICKOS_ARMV6M_TRAP_FRAME,
              "PSP_GUARD's F_TRAP_FRAME prices {r4-r11}: eight words");
// The figure the red-zone gate scrapes as the SVC class's non-measured half: the frame the
// exception return unstacks, less svc_trampoline's scratch push and the exception pair that
// can preempt it before it reaches ctx.kernel_sp. The STKALIGN pad is absent because it
// cancels, which armv6m_trap_stack.h derives. PENDSV's half is KICKOS_ARMV6M_TRAP_FRAME.
static_assert(KICKOS_ARMV6M_TRAP_NEST_SVC
                  == 8 + 32 + KICKOS_ARMV6M_TRAP_FRAME - 32,
              "the SVC window is the unstack credit, the trampoline's scratch push, a "
              "preempting hardware frame and the PendSV block that tail-chains below it");
// The kernel block's structural half: the continuation header svc_trampoline lays at the
// block top, the pad a preempting entry spends there (it does NOT cancel, the frames above
// being the compiler's), that hardware frame and the PendSV block below it.
static_assert(KICKOS_ARMV6M_TRAP_NEST_SVCK
                  == 16 + 4 + 32 + KICKOS_ARMV6M_TRAP_FRAME,
              "the SVCK structural half is the continuation header, the STKALIGN pad, a "
              "preempting hardware frame and the PendSV block that tail-chains below it");
// NOTHING DESCENDS ON THE THREAD STACK AT THE SVC SITE any more: svc_trampoline moves SP to
// ctx.kernel_sp before it calls anything, so the dispatch is measured as SVCK. A nonzero
// figure here needs roots in tests/static/trap_redzone_roots.txt, where the SVC class
// declares NONE.
static_assert(KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_SVC == 0,
              "a nonzero SVC descent needs roots in tests/static/trap_redzone_roots.txt");
// The PENDSV class charges the push alone. A claim about handler mode rather than a
// measurement: ARMv6-M forces SP_main there, so everything PendSV_Handler calls runs on
// the MSP.
static_assert(KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_PENDSV == 0,
              "a nonzero PendSV descent needs roots in tests/static/trap_redzone_roots.txt");
static_assert(KICKOS_ARMV6M_TRAP_NEED_SVC > KICKOS_ARMV6M_TRAP_NEED_PENDSV,
              "SVC_Handler charges the larger of the two figures for both of its arms, so "
              "it must dominate the fastpath arm's own {r4-r11} push");
// The floor must DOMINATE the worst-case red zone, or a thread spawned at the floor passes
// the spawn check and is then refused by the guard on every syscall it makes. This
// assertion covers every armv6m board.
//
// STRICTER THAN THE RED-ZONE GATE'S OWN FLOOR CLAUSE, and it has to be: the guard bounds
// the room below the PSP, and the hardware has already spent bytes ABOVE it before any
// handler runs. A thread trapping with an empty floor-sized stack therefore offers the
// guard only KICKOS_MIN_STACK_SIZE minus that frame. 32 is the basic frame, and STKALIGN
// washes out: a 4-mod-8 SP makes entry spend 36 but hands the trampoline back the same 36.
// The gate models no such term, which is why it lives here.
// THE DEATH PATH, two classes: the fault and slay stubs relocate to the thread's own kernel
// block, so the BLOCK above its canary is what holds them; kickos_thread_return does not
// relocate, so the spawn floor is what holds it.
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t)
                  >= KICKOS_ARMV6M_TRAP_NEST_EXIT + KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_EXITK,
              "the kernel block cannot hold the relocated death path plus its canary word");
static_assert(KICKOS_MIN_STACK_SIZE
                  >= KICKOS_ARMV6M_TRAP_NEST_EXIT + KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_RET,
              "the spawn floor cannot hold a privileged thread's entry return");
static_assert(KICKOS_MIN_STACK_SIZE >= KICKOS_ARMV6M_TRAP_NEED_SVC + 32,
              "KICKOS_MIN_STACK_SIZE is below the armv6m syscall red zone plus the "
              "exception frame entry spends above it: raise the per-arch default in "
              "Kconfig, never the red zone, which is a measurement");
// ARMv6-M keeps SP 8-byte aligned at every public interface (AAPCS), and exception entry
// clears bit 2 of the banked SP, so a kernel stack whose SIZE is not a multiple of 8
// puts its top off that boundary.
static_assert(KICKOS_KERNEL_STACK_SIZE % 8 == 0,
              "KICKOS_KERNEL_STACK_SIZE must be a multiple of 8 on this arch, or a "
              "kernel stack's top does not land on the alignment every frame on it "
              "assumes");
// THE TRAMPOLINE HAS NOWHERE ELSE TO BUILD. With no block seated, every syscall takes
// svc_trampoline's refusal path and the first one a thread makes ends the system, so this
// arch cannot be configured without the blocks.
// KCONFIG NOW FORECLOSES THE 0 CASE rather than leaving this assert to catch it: this arch
// selects ARCH_KERNEL_STACKS_MANDATORY, which puts `range 1 1` on the knob, so a defconfig
// asking for 0 is refused by name at configure. Kept because it states the arch's requirement
// at the point of use, and because a `select` removed by accident should fail loudly here.
static_assert(KICKOS_KERNEL_STACKS != 0,
              "armv6m's syscall trap runs every dispatch on ctx.kernel_sp");
// THE CEILING MUST COVER ITS OWN REQUIREMENT. The deepest a syscall drives a kernel block
// is the continuation header, the dispatch below it, and the hardware frame plus PendSV
// block a preemption takes at that depth. The lowest word of the block is the overflow
// canary (kernel/thread/thread.cc), so the requirement has to fit ABOVE it: a ceiling that
// merely equals the requirement reports an overflow on the deepest legitimate descent, and
// one below it overflows the block for real. This assert covers every armv6m board.
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t) >= KICKOS_ARMV6M_TRAP_NEED_SVCK,
              "KICKOS_KERNEL_STACK_SIZE is below the armv6m syscall kernel-stack "
              "requirement plus its canary word: raise the per-arch default in Kconfig, "
              "never the depth, which is a measurement");

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
    // PendSV-saved block {r4-r11}.
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

    // No kernel stack is allocated yet, so it is seated at 0 rather than left as whatever
    // the TCB slab last held. A trusted entry that finds 0 here has nothing to transfer to,
    // which is the state every thread is in until the allocator lands.
    ctx->kernel_sp = 0;
}

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

// The fabricated first frame already lands at the stack top with CONTROL.nPRIV carrying
// privilege, so a rebuild is that same fabrication.
void arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                       void* stack_base, size_t stack_size)
{
    // kernel_sp SURVIVES THE REBUILD, and arch_context_init clearing it is right for a fresh
    // TCB and wrong here. This ctx belongs to a LIVE POOL THREAD being redirected onto its
    // slay stub: its block is seated by pool slot and does not move, and thread_create is the
    // only other writer. Cleared, the thread carries kernel_sp 0 through its own teardown,
    // which is the state svc_trampoline's .Lsvc_nokstack arm calls unreachable.
    uint32_t const kernel_sp = ctx->kernel_sp;
#if KICKOS_KERNEL_STACKS
    // THE SLAY STUB IS REBUILT ON THE THREAD'S OWN KERNEL BLOCK, so no privileged frame is
    // fabricated on memory the thread or a domain sibling can write. The frame goes at the
    // block TOP, which discards whatever dispatch frames the block held: the thread is dying
    // and nothing resumes it, and it is what keeps the block requirement the MAX of the
    // dispatch and exit classes rather than their sum.
    //
    // THE USER BOUNDS ARE PRESERVED ACROSS IT. arch_context_init derives stack_lo and
    // stack_hi from what it is handed, so handing it the block would leave the context
    // describing kernel .bss as this thread's stack.
    //
    // THEIR ONLY READERS ARE THE FOUR switch.S GUARDS, and NOT the fault record: that reads
    // Thread::stack_base and Thread::stack_size, which this function never touches. So no
    // runtime path can observe the restore being absent, every guard testing ctx.kernel_sp
    // first and taking the block leg for this frame. tests/static/check_death_stack_seating.sh
    // is what holds it, for exactly that reason.
    if (kernel_sp != 0)
    {
        uint32_t const lo = ctx->stack_lo;
        uint32_t const hi = ctx->stack_hi;
        void* const block = reinterpret_cast<void*>(
            static_cast<uintptr_t>(kernel_sp) - KICKOS_KERNEL_STACK_SIZE);
        arch_context_init(ctx, entry, nullptr, block, KICKOS_KERNEL_STACK_SIZE, 1);
        ctx->stack_lo = lo;
        ctx->stack_hi = hi;
        ctx->kernel_sp = kernel_sp;
        return;
    }
#endif
    arch_context_init(ctx, entry, nullptr, stack_base, stack_size, 1);
    ctx->kernel_sp = kernel_sp;
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

// --- Interrupt controller (NVIC). The crit section is PRIMASK and masks every
// configurable line, so a device line's priority does not gate it and unmask enables the
// line alone. mask/inject are core-generic (arch_arm_common.cc). -------------------
void arch_irq_unmask(int line)
{
    if (line < 0)
    {
        return;
    }
    unsigned l = static_cast<unsigned>(line);
    // Latch-and-coalesce: any NVIC pending latched while the line was masked is
    // PRESERVED across enable and fires through the normal ISR path the instant ISER is
    // set. The dsb drains a preceding device-flag clear, so a level source that is
    // genuinely deasserted does not re-latch.
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

// --- Fault reporting: the shared HardFault handler. HardFault is the whole fault
// taxonomy on v6-M and the core exposes no fault-status registers, so the stacked frame
// is the entire dump. ------------------------------------------------------------
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
// with EXC_RETURN, to the C reporter. Naked so no prologue perturbs the SP before it is
// read. v6-M conditionalises by branch, so the stack select is movs/tst/beq/mrs.
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

// Called from switch.S when the running thread's live PSP lacks room BELOW it for the
// {r4-r11} block the push is about to write there.
//
// Runs in handler mode on the MSP, and contains the system rather than the write: the
// only frame and PSP a resume could use are the ones the guard just refused.
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

// Called from switch.S (svc_trampoline) when the calling thread has no kernel block seated,
// so the transfer has nowhere to build. Runs privileged in THREAD mode ON THE MSP,
// .Lsvc_nokstack clearing CONTROL.SPSEL before the branch and deriving there why the PSP
// cannot carry it. `psp` is the thread's own, computed before that clear. Contains the
// system rather than the dispatch: nothing has run yet.
void kickos_armv6m_no_kernel_stack(uint32_t psp)
{
    kpanic_enter();
    ::kickos::kprintf("\n=== ARMV6M EXCEPTION (no kernel stack) ===\n");
#if KICKOS_PANIC_DUMP
    ::kickos::kprintf("  in svc_trampoline PSP=0x%x\n", static_cast<unsigned>(psp));
#else
    (void)psp;
#endif
    kfault_terminate();
}

// Install the system-handler priorities (SHPR is word-access only on v6-M). Called by
// the chip's arch_init.
void kickos_armv6m_init(void)
{
    reg32(SCB_SHPR2) = (reg32(SCB_SHPR2) & 0x00FFFFFFu) | (PRIO_SVCALL << 24);
    uint32_t shpr3 = reg32(SCB_SHPR3) & 0x0000FFFFu;
    shpr3 |= (PRIO_PENDSV << 16) | (PRIO_SYSTICK << 24);
    reg32(SCB_SHPR3) = shpr3;
}

}
