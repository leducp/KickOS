// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ARMv6-M (Cortex-M0/M0+) arch backend: the arch.h seam parts this profile does not share
// with ARMv7-M.

#include <kickos/arch/arch.h>
#include "ctx_redirect.h"
#include <kickos/arch/armv6m_fault_frame.h>
#include <kickos/arch/armv6m_trap_stack.h>
#include <kickos/diag.h>

#include "regs.h"
#include <kickos/trace/record.h>

#include <stddef.h>

// A mismatch mislabels every SESSION record.
static_assert(KICKOS_TRACE_ARCH == kickos::trace::ARCH_ARMV6M,
              "KICKOS_TRACE_ARCH does not match ArchId::ARCH_ARMV6M for armv6m");

namespace kickos
{
    void kprintf(char const* fmt, ...);
}
extern "C" void kpanic_enter(void);
extern "C" void kfault_terminate(void) __attribute__((noreturn));

// 0 keeps only the one-line fault marker.
#ifndef KICKOS_PANIC_DUMP
#define KICKOS_PANIC_DUMP 1
#endif

static_assert(offsetof(struct arch_context, sp) == 0, "switch.S expects ctx.sp @0");
static_assert(offsetof(struct arch_context, npriv) == 4, "switch.S expects ctx.npriv @4");
static_assert(offsetof(struct arch_context, resting_npriv) == 8,
              "switch.S expects ctx.resting_npriv @8");
// The PSP bounds guard reads these as plain displacements. The telemetry field is last so no
// build posture moves them.
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
    // gas cannot count the registers in an stmia, so F_TRAP_FRAME is priced against this.
    constexpr size_t CALLEE_BLOCK_WORDS = 8;
}
// The reporter's own array (kernel/init/console.cc); the frame term is the hardware frame a
// HardFault stacks there.
static_assert(KICKOS_ARMV6M_PANIC_FRAME == KICKOS_ARMV6M_TRAP_FRAME,
              "the panic frame term is the hardware frame, which switch.S cannot compute");
static_assert(KICKOS_PANIC_STACK_SIZE >= KICKOS_ARMV6M_PANIC_FRAME + KICKOS_ARMV6M_PANIC_DEPTH,
              "KICKOS_PANIC_STACK_SIZE is below what this arch's panic reporter descends");

static_assert(CALLEE_BLOCK_WORDS * sizeof(uint32_t) == KICKOS_ARMV6M_TRAP_FRAME,
              "PSP_GUARD's F_TRAP_FRAME prices {r4-r11}: eight words");
// The SVC class's structural half, scraped by the red-zone gate and derived in
// armv6m_trap_stack.h.
static_assert(KICKOS_ARMV6M_TRAP_NEST_SVC
                  == 8 + 32 + KICKOS_ARMV6M_TRAP_FRAME - 32,
              "the SVC window is the unstack credit, the trampoline's scratch push, a "
              "preempting hardware frame and the PendSV block that tail-chains below it");
static_assert(KICKOS_ARMV6M_TRAP_NEST_SVCK
                  == 16 + 4 + 32 + KICKOS_ARMV6M_TRAP_FRAME,
              "the SVCK structural half is the continuation header, the STKALIGN pad, a "
              "preempting hardware frame and the PendSV block that tail-chains below it");
// svc_trampoline moves SP to ctx.kernel_sp before any call, so the SVC class declares no roots.
static_assert(KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_SVC == 0,
              "a nonzero SVC descent needs roots in tests/static/trap_redzone_roots.txt");
static_assert(KICKOS_MAX_SPAWN_GRANTS <= 9,
              "KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_SVCK was measured at a spawn staging 9 grants on "
              "the kernel block: re-measure with tests/static/check_trap_redzone.sh first");
// Handler mode forces SP_main, so everything PendSV_Handler calls runs on the MSP.
static_assert(KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_PENDSV == 0,
              "a nonzero PendSV descent needs roots in tests/static/trap_redzone_roots.txt");
static_assert(KICKOS_ARMV6M_TRAP_NEED_SVC > KICKOS_ARMV6M_TRAP_NEED_PENDSV,
              "SVC_Handler charges the larger of the two figures for both of its arms, so "
              "it must dominate the fastpath arm's own {r4-r11} push");
// The fault and slay stubs run on the thread's kernel block.
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t)
                  >= KICKOS_ARMV6M_TRAP_NEST_EXIT + KICKOS_ARMV6M_TRAP_KERNEL_DEPTH_EXITK,
              "the kernel block cannot hold the relocated death path plus its canary word");
// The floor must DOMINATE the worst-case red zone, or a thread spawned at the floor passes
// the spawn check and is refused by the guard on every syscall. The hardware spends the entry
// frame ABOVE the PSP before the guard runs, so a floor-sized empty stack offers the guard
// only KICKOS_MIN_STACK_SIZE minus that frame.
static_assert(KICKOS_MIN_STACK_SIZE
                  >= KICKOS_ARMV6M_TRAP_NEED_SVC + KICKOS_ARMV6M_TRAP_ENTRY_FRAME,
              "KICKOS_MIN_STACK_SIZE is below the armv6m syscall red zone plus the "
              "exception frame entry spends above it: raise the per-arch default in "
              "Kconfig, never the red zone, which is a measurement");
static_assert(KICKOS_KERNEL_STACK_SIZE % 8 == 0,
              "KICKOS_KERNEL_STACK_SIZE must be a multiple of 8 on this arch, or a "
              "kernel stack's top does not land on the alignment every frame on it "
              "assumes");
// ARCH_KERNEL_STACKS_MANDATORY puts `range 1 1` on the knob; this fires if that select is
// dropped.
static_assert(KICKOS_KERNEL_STACKS != 0,
              "armv6m's syscall trap runs every dispatch on ctx.kernel_sp");
// The lowest word of the block is the overflow canary (kernel/thread/thread.cc), so the
// requirement must fit ABOVE it.
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t) >= KICKOS_ARMV6M_TRAP_NEED_SVCK,
              "KICKOS_KERNEL_STACK_SIZE is below the armv6m syscall kernel-stack "
              "requirement plus its canary word: raise the per-arch default in Kconfig, "
              "never the depth, which is a measurement");

namespace
{
    using namespace kickos::arm;
    using namespace kickos::armv6m;
}

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

    *(--sp) = 0x01000000u;                              // xPSR (Thumb bit)
    *(--sp) = reinterpret_cast<uint32_t>(entry) & ~1u;  // PC = entry
    *(--sp) = ret;                                      // LR: entry returns here
    *(--sp) = 0;                                        // r12
    *(--sp) = 0;                                        // r3
    *(--sp) = 0;                                        // r2
    *(--sp) = 0;                                        // r1
    *(--sp) = reinterpret_cast<uint32_t>(arg);          // r0 = arg
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

    // The PSP guard in PendSV and the SVC fastpath checks against these; `top` is the
    // aligned high edge.
    ctx->stack_lo = reinterpret_cast<uint32_t>(stack_base);
    ctx->stack_hi = static_cast<uint32_t>(top);

    // ctx->kernel_sp IS UNTOUCHED: thread_create seats it before this call, and zero is its
    // no-block marker.
}

void arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                       void* stack_base, size_t stack_size)
{
    arch_ctx_redirect_to_block(ctx, entry, stack_base, stack_size);
}

// --- Fault reporting. HardFault is the whole fault taxonomy on v6-M and there are no
// fault-status registers, so the stacked frame is the entire dump. ------------
void kickos_armv6m_fault_report(uint32_t* frame, uint32_t exc_return)
{
    // The naked handler reaches here by `bx`, so this function's own return IS the
    // exception return. Nothing may print above this: kpanic_enter's console reclaim is
    // permanent and this fault is survivable.
    if (kickos_fault_kill_thread(frame))
    {
        return;
    }
    kickos_fault_report_begin();
    kpanic_enter();
    ::kickos::kprintf("\n=== HARD FAULT ===\n");
#if KICKOS_PANIC_DUMP
    char const* stk = "MSP";
    if (exc_return & 0x4u)
    {
        stk = "PSP";
    }
    if (armv6m_fault_frame_readable(frame, exc_return))
    {
        ::kickos::kprintf(KDIAG_F_ARM_REGS1, frame[6], frame[5], frame[7], stk);
        ::kickos::kprintf(KDIAG_F_ARM_REGS2, frame[0], frame[1], frame[2], frame[3], frame[4]);
    }
    else
    {
        ::kickos::kprintf(KDIAG_F_ARM_FRAME_FOREIGN, reinterpret_cast<uint32_t>(frame), stk);
    }
#else
    (void)frame;
    (void)exc_return;
#endif
    kfault_terminate();
}

// Naked so no prologue perturbs SP before it is read.
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

// SHPR is word-access only on v6-M.
void kickos_armv6m_init(void)
{
    reg32(SCB_SHPR2) = (reg32(SCB_SHPR2) & 0x00FFFFFFu) | (PRIO_SVCALL << 24);
    uint32_t shpr3 = reg32(SCB_SHPR3) & 0x0000FFFFu;
    shpr3 |= (PRIO_PENDSV << 16) | (PRIO_SYSTICK << 24);
    reg32(SCB_SHPR3) = shpr3;
}

}
