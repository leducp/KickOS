// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ARMv7-M arch backend: the core-generic half of the arch.h seam. Context switch and
// syscall trap are in switch.S; the chip layer (arch/arm/chip/*) supplies arch_init,
// arch_console_write, SystemCoreClock and the linker script naming the user-RAM region.

#include <kickos/arch/arch.h>
#include <kickos/diag.h>
#include <kickos/units.h> // _s literal (== 1e9 ns)

#include "regs.h"
#include <kickos/arch/armv7m_trap_stack.h> // the figures switch.S's PSP guard enforces
#include <kickos/trace/record.h> // ArchId: pin this build's trace-arch id to this backend

#include <stddef.h> // offsetof
#include <stdint.h>

// A mismatch mislabels every SESSION record; the id comes from the CMake ladder and this
// chip's caps.cmake.
static_assert(KICKOS_TRACE_ARCH == kickos::trace::ARCH_ARMV7M,
              "KICKOS_TRACE_ARCH does not match ArchId::ARCH_ARMV7M for armv7m");

// switch.S hard-codes these arch_context field offsets, so a reorder would corrupt the
// saved SP or privilege state.
static_assert(offsetof(struct arch_context, sp) == 0, "switch.S expects ctx.sp @0");
static_assert(offsetof(struct arch_context, npriv) == 4, "switch.S expects ctx.npriv @4");
static_assert(offsetof(struct arch_context, resting_npriv) == 8,
              "switch.S expects ctx.resting_npriv @8");
// The PSP bounds guard reads these as plain displacements, so a reorder has it compare a
// PSP against the wrong words. The telemetry field is last, which keeps both offsets the
// same in every build posture.
static_assert(offsetof(struct arch_context, stack_lo) == KICKOS_ARMV7M_CTX_OFF_STACK_LO,
              "switch.S reads ctx.stack_lo at KICKOS_ARMV7M_CTX_OFF_STACK_LO");
static_assert(offsetof(struct arch_context, stack_hi) == KICKOS_ARMV7M_CTX_OFF_STACK_HI,
              "switch.S reads ctx.stack_hi at KICKOS_ARMV7M_CTX_OFF_STACK_HI");
// Unconditional even at KICKOS_KERNEL_STACKS 0: the field is in the struct on every build,
// so the offsets after it must not move with the posture.
static_assert(offsetof(struct arch_context, kernel_sp) == KICKOS_ARMV7M_CTX_OFF_KERNEL_SP,
              "svc_trampoline and PendSV_Handler load ctx.kernel_sp at F_CTX_KERNEL_SP");

// gas cannot count the registers in an stmdb, so the frame halves are priced here against
// the register lists the two pushes carry.
static_assert(KICKOS_ARMV7M_TRAP_FRAME == 9u * sizeof(uint32_t),
              "PSP_GUARD prices {r4-r11, EXC_RETURN}: nine words");
static_assert(KICKOS_ARMV7M_TRAP_FRAME_FP == 16u * sizeof(uint32_t),
              "the FP term prices {s16-s31}: sixteen words");
static_assert(KICKOS_ARMV7M_TRAP_FRAME_MAX
                  == KICKOS_ARMV7M_TRAP_FRAME + KICKOS_ARMV7M_TRAP_FRAME_FP,
              "FRAME_MAX is the worst-case push and is what the red-zone gate scrapes");
// One hoisted guard charges the same figure for both arms, so the SVC figure must dominate
// the fastpath arm's own worst-case push.
static_assert(KICKOS_ARMV7M_TRAP_NEST_SVC >= KICKOS_ARMV7M_TRAP_FRAME_MAX,
              "the SVC site must cover the fastpath arm's FP-live push, which it guards too");
// The SVC window: the frame the exception return unstacks, less svc_trampoline's own
// eight-byte prologue and the exception pair that can preempt it. No STKALIGN term: it
// cancels here, and the 8 is the same under both entry designs, as armv7m_trap_stack.h
// derives.
static_assert(KICKOS_ARMV7M_TRAP_NEST_SVC
                  == 8 + 104 + KICKOS_ARMV7M_TRAP_FRAME_MAX - 32,
              "the SVC window is the unstack credit, the trampoline's prologue, a "
              "preempting hardware frame and the PendSV block that tail-chains below it");
#if !KICKOS_KERNEL_STACKS
// The same window with the dispatch inside it. The pad stops cancelling once the compiler's
// frames stand between the trampoline and the preemption point, so the two figures differ
// by that pad and nothing else. Asserted because the gate scrapes both as plain immediates
// and so cannot catch a drift between them.
static_assert(KICKOS_ARMV7M_TRAP_NEST_SVC_DISPATCH == KICKOS_ARMV7M_TRAP_NEST_SVC + 4,
              "the unconverted SVC window is the converted one plus the STKALIGN pad a "
              "preempting entry spends below a chain of compiler frames");
#endif
#if KICKOS_KERNEL_STACKS
// The kernel block's structural half. The STKALIGN pad does NOT cancel here, the frames
// above it being the compiler's.
static_assert(KICKOS_ARMV7M_TRAP_NEST_SVCK
                  == 16 + 4 + 104 + KICKOS_ARMV7M_TRAP_FRAME_MAX,
              "the SVCK structural half is the continuation header, the STKALIGN pad, a "
              "preempting hardware frame and the PendSV block that tail-chains below it");
// SVCK is the same dispatch as SVC with the panic tail counted rather than excluded, so it
// cannot be the smaller. The gate scrapes both as plain immediates, so a swap between them
// is not a typo the compiler catches.
static_assert(KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVCK >= KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVC,
              "the tail-counted dispatch depth is below the tail-excluded one");
#endif
// Handler mode rather than a measurement: ARMv7-M forces SP_main there, so everything
// PendSV_Handler calls runs on the MSP.
static_assert(KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_PENDSV == 0,
              "a nonzero PendSV descent needs roots in tests/static/trap_redzone_roots.txt");
static_assert(KICKOS_ARMV7M_TRAP_NEED_SVC > KICKOS_ARMV7M_TRAP_NEED_PENDSV,
              "the SVC site must charge more than the switcher: it keeps running on the PSP");
// The floor must DOMINATE the worst-case red zone, or a thread spawned at the floor passes
// the spawn check and is then refused by the guard on every syscall it makes. Stricter
// than the red-zone gate's own floor clause: the hardware spends bytes ABOVE the PSP before
// any handler runs, so a floor-sized empty stack offers the guard only
// KICKOS_MIN_STACK_SIZE minus that frame.
//
// The entry frame and not the descent is the binding term. What the descent needs cancels
// across the two postures, 32 + 836 and 104 + 764 both being 868, a wider entry frame being
// handed straight back when the exception return unstacks it. What the guard demands does
// not: it asks for NEED_SVC unconditionally, so the requirement is NEED_SVC plus whatever
// entry spent, and the FP-live 104 is the worse of the two. That leaves the guard 72 bytes
// stricter than physics there, the price of not branching on FPCA at the SVC site.
#if defined(__ARM_FP)
static_assert(KICKOS_MIN_STACK_SIZE >= KICKOS_ARMV7M_TRAP_NEED_SVC + 104,
              "KICKOS_MIN_STACK_SIZE is below the armv7m syscall red zone plus the "
              "FP-live exception frame entry spends above it: raise the per-arch default in "
              "Kconfig, never the red zone, which is a measurement");
#else
static_assert(KICKOS_MIN_STACK_SIZE >= KICKOS_ARMV7M_TRAP_NEED_SVC + 32,
              "KICKOS_MIN_STACK_SIZE is below the armv7m syscall red zone plus the "
              "exception frame entry spends above it: raise the per-arch default in "
              "Kconfig, never the red zone, which is a measurement");
#endif

// ARMv7-M keeps SP 8-byte aligned at every public interface (AAPCS), so a kernel stack
// whose SIZE is not a multiple of 8 puts its top off that boundary.
static_assert(KICKOS_KERNEL_STACK_SIZE % 8 == 0,
              "KICKOS_KERNEL_STACK_SIZE must be a multiple of 8 on this arch, or a "
              "kernel stack's top does not land on the alignment every frame on it "
              "assumes");
#if KICKOS_KERNEL_STACKS
// PendSV_Handler loads the block's size with movw to test a PSP against it.
static_assert(KICKOS_KERNEL_STACK_SIZE <= 0xFFFF,
              "PendSV_Handler's kernel-block leg loads KICKOS_KERNEL_STACK_SIZE with movw");
// The lowest word of the block is the overflow canary (kernel/thread/thread.cc), so the
// requirement must fit ABOVE it: a ceiling that merely equals the requirement reports an
// overflow on the deepest legitimate descent.
//
// Both sides resolve per KICKOS_TELEMETRY, so this prices the posture the image compiles:
// NEED_SVCK takes the telemetry tail's depth through armv7m_trap_stack.h and the Kconfig
// default for the ceiling carries the matching figure, so a board raising one without the
// other fails here rather than at run time.
//
// EXITK is the fault and slay stubs on the thread's own kernel block. Its two siblings, the
// kstacks=0 fallback and the entry-return residual, sit BELOW this #if and must stay there:
// this guard is false on the very boards they describe.
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t)
                  >= KICKOS_ARMV7M_TRAP_NEST_EXIT + KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_EXITK,
              "the kernel block cannot hold the relocated death path plus its canary word");
static_assert(KICKOS_KERNEL_STACK_SIZE - sizeof(uint32_t) >= KICKOS_ARMV7M_TRAP_NEED_SVCK,
              "KICKOS_KERNEL_STACK_SIZE is below the armv7m syscall kernel-stack "
              "requirement plus its canary word: raise the per-arch default in Kconfig, "
              "never the depth, which is a measurement");
#endif
// The death path's thread-stack half. RET is kickos_thread_return, which relocates under
// NEITHER entry design, so the floor holds it on every board. EXIT is the fallback the
// presets with no block take, so it is asserted where the block guard is FALSE.
#if !KICKOS_KERNEL_STACKS
static_assert(KICKOS_MIN_STACK_SIZE
                  >= KICKOS_ARMV7M_TRAP_NEST_EXIT + KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_EXIT,
              "the spawn floor cannot hold the death path where no kernel block is seated");
#endif
static_assert(KICKOS_MIN_STACK_SIZE
                  >= KICKOS_ARMV7M_TRAP_NEST_EXIT + KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_RET,
              "the spawn floor cannot hold a privileged thread's entry return");
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
static_assert(offsetof(struct arch_context, trace_tid) == KICKOS_ARMV7M_CTX_OFF_TRACE_TID,
              "switch.S telemetry hook reads ctx.trace_tid at F_CTX_TRACE_TID");
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
    // CONTROL.nPRIV: 0 = privileged, 1 = unprivileged.
    uint32_t npriv = 1;
    if (privileged)
    {
        npriv = 0;
    }
    ctx->npriv = npriv;
    ctx->resting_npriv = npriv;

    // PendSV and SVC_Handler check the live PSP against these before either pushes the
    // {r4-r11, EXC_RETURN} block through it. `top` is the aligned high edge the first frame
    // sits below, so a running thread's PSP stays in [stack_lo, stack_hi).
    ctx->stack_lo = reinterpret_cast<uint32_t>(stack_base);
    ctx->stack_hi = static_cast<uint32_t>(top);

    // 0 means no block seated, which is what svc_trampoline's refusal path keys on; the
    // TCB slab would otherwise hand this field whatever it last held.
    ctx->kernel_sp = 0;
}

#if defined(KICKOS_ARCH_HAS_IPC_FASTPATH) && KICKOS_ARCH_HAS_IPC_FASTPATH
// The result has to be seated where the restore reloads r4 from: ctx->sp is the base of the
// {r4-r11, EXC_RETURN} block. r4, not the AAPCS r0, is the register the trap's own ABI
// answers in (arch_syscall_reg in switch.S).
void arch_ctx_set_syscall_result(struct arch_context* ctx, uint32_t result)
{
    reinterpret_cast<uint32_t*>(ctx->sp)[0] = result;
}
#endif

// The fabricated frame carries EXC_RETURN 0xFFFFFFFD (thread mode, PSP, NON-FP frame), so
// the rebuild also RESETS the frame format: a thread that had an extended FP frame stacked
// resumes on a plain 8-word one. Sound only because every frame it held is discarded here.
void arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                       void* stack_base, size_t stack_size)
{
    // kernel_sp SURVIVES THE REBUILD. arch_context_init clears it, which is right for a
    // fresh TCB and wrong for a live pool thread whose block is seated by slot: cleared,
    // the thread carries 0 through its own teardown and every syscall on the way takes
    // svc_trampoline's .Lsvc_nokstack arm.
    uint32_t const kernel_sp = ctx->kernel_sp;
#if KICKOS_KERNEL_STACKS
    // The stub is rebuilt on the thread's own kernel block, so no privileged frame is
    // fabricated on memory the thread or a domain sibling can write. The frame goes at the
    // block TOP, discarding whatever dispatch frames it held, which is what keeps the block
    // requirement the MAX of the dispatch and exit classes rather than their sum.
    //
    // stack_lo and stack_hi are saved and put back because arch_context_init derives them
    // from what it is handed, and handing it the block would leave the context describing
    // kernel .bss as this thread's stack.
    // tests/static/check_death_stack_seating.sh holds this shape.
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

// --- Critical section: raise BASEPRI to the kernel lock threshold -----------
arch_irq_state_t arch_irq_save(void)
{
    uint32_t prev;
    __asm volatile("mrs %0, basepri" : "=r"(prev));
    // Lower BASEPRI value = stronger mask, 0 = no mask. Already masking at least as
    // strongly as the lock means the section is in effect, so the write and its barriers
    // are skipped; a weaker prev (a device band 0x30) still raises to the lock below.
    if (prev != 0 and prev <= PRIO_LOCK_BASEPRI)
    {
        return prev;
    }
    __asm volatile("msr basepri, %0" ::"r"(PRIO_LOCK_BASEPRI) : "memory");
    // Raising BASEPRI is not self-synchronizing: without these an interrupt could be taken
    // on the following instruction under the OLD mask (ARMv7-M ARM, "Barriers").
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
    return prev;
}

void arch_irq_restore(arch_irq_state_t state)
{
    __asm volatile("msr basepri, %0" ::"r"(state) : "memory");
}

// --- Monotonic clock ---------------------------------------------------------
// arch_clock_now is a REQUIRED chip contract, over a dedicated peripheral timer. The DWT is
// debug-domain (gated by DEMCR.TRCENA, lockable on Cortex-M7, absent under QEMU), so this
// arch supplies no fallback and a board omitting its definition fails at link time rather
// than hanging on its first sleep. The one-shot SysTick timer is core-generic
// (arch_arm_common).

// --- Interrupt controller (NVIC). mask/inject are core-generic (arm/common); unmask is
// arch-specific because it programs the BASEPRI-maskable priority band the crit section
// relies on.
void arch_irq_unmask(int line)
{
    if (line < 0)
    {
        return;
    }
    unsigned l = static_cast<unsigned>(line);
    // Priority into the kernel-maskable band BEFORE enabling: NVIC IPR resets to 0x00 and
    // the BASEPRI (0x20) critical section masks only priorities numerically >= 0x20, so
    // without this a device IRQ preempts an IrqLock-held section (regs.h band).
    reinterpret_cast<volatile uint8_t*>(NVIC_IPR0)[l] = static_cast<uint8_t>(PRIO_DEVICE);
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

// --- Kernel-facing ISR entries ----------------------------------------------
// The exception number in IPSR is 16 + external-line.
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

// --- Fault reporting: a shared HardFault, which the chip vectors also route
// MemManage/BusFault/UsageFault to.
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
// The frame is always on the USER stack under either entry design, which is why the test
// below is the thread-stack one and not its kernel-block twin: the hardware stacks it at
// the PSP with the PRE-exception privilege, and an accepted fault here is by definition one
// taken with nPRIV set, which the dispatch never is.
bool arch_fault_is_user_thread(void* frame)
{
    uint32_t control;
    __asm volatile("mrs %0, control" : "=r"(control));
    if ((control & 1u) == 0u)
    {
        return false;
    }
    // MSTKERR/MUNSTKERR (CFSR bits 4/3) and STKERR/UNSTKERR (bits 12/11) mean the hardware
    // aborted mid-stacking, so `frame` addresses memory the frame was never written to and
    // f[7] below would be whatever RAM already held. A stack overflow arrives this way.
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
    uint32_t const cfsr = kickos::arm::reg32(0xE000ED28);
    uint32_t const hfsr = kickos::arm::reg32(0xE000ED2C);
    uintptr_t addr = 0;
    int addr_valid = 0;
    // MMFAR/BFAR hold a stale address unless the matching VALID bit is set (MMARVALID =
    // CFSR bit 7, BFARVALID = bit 15).
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
    // Write-1-to-clear and sticky: a bit left set mislabels the NEXT thread's fault.
    kickos::arm::reg32(0xE000ED28) = cfsr;
    kickos::arm::reg32(0xE000ED2C) = hfsr;

    // The stub runs at the top of this thread's stack, and the frame is NOT relocated to
    // get there: with lazy FP stacking the EXC_RETURN still in the handler's LR decides
    // whether the CPU unstacks a basic 8-word or an extended 26-word frame (ARMv7-M ARM
    // B1.5.7), and a frame moved somewhere EXC_RETURN disagrees with is popped at the wrong
    // size out of the wrong memory. So the hardware pops where it stacked and the shim's
    // first instruction, reached only after that pop, moves SP. r0 carries the new SP
    // because it is the frame's own first word and this thread is dying.
    //
    // The stack-realign bit 9 is left alone: it belongs to the pop, which still happens at
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
    // Exception return does not restore CONTROL, so clearing nPRIV here is what makes the
    // stub privileged. SPSEL is the bit handler mode ignores; nPRIV is not.
    uint32_t control;
    __asm volatile("mrs %0, control" : "=r"(control));
    __asm volatile("msr control, %0" ::"r"(control & ~1u));
    __asm volatile("isb" ::: "memory");
}

// From switch.S (PendSV and SVC_Handler), when the running thread's live PSP lacks room
// BELOW it for the {r4-r11, EXC_RETURN} block about to be pushed there. Runs in handler
// mode on the MSP and does not return: the only frame and PSP a resume could use are the
// ones the guard just refused.
void kickos_armv7m_bad_psp(uint32_t psp, uint32_t need, uint32_t lo, uint32_t hi)
{
    kpanic_enter();
#if KICKOS_PANIC_DUMP
    // Re-derived rather than passed: one guard serves all three legs.
    char const* why = "no room below";
    if (psp < lo)
    {
        why = "under stack_lo";
    }
    else if (psp >= hi)
    {
        why = "at or above stack_hi";
    }
    // Which guarded push refused: nothing in the arguments separates the two sites, so it
    // comes from ICSR.VECTACTIVE.
    uint32_t const vect = kickos::arm::reg32(0xE000ED04) & 0x1FFu;
    char const* site = "handler";
    if (vect == 11u)
    {
        site = "SVCall";
    }
    else if (vect == 14u)
    {
        site = "PendSV";
    }
    ::kickos::kprintf("\n=== ARMV7M EXCEPTION (wild PSP: %s) ===\n", why);
    ::kickos::kprintf("  in %s PSP=0x%x need=%u stack=[0x%x,0x%x)\n", site,
                      static_cast<unsigned>(psp), static_cast<unsigned>(need),
                      static_cast<unsigned>(lo), static_cast<unsigned>(hi));
#else
    (void)psp;
    (void)need;
    (void)lo;
    (void)hi;
    ::kickos::kprintf("\n=== ARMV7M EXCEPTION (wild PSP) ===\n");
#endif
    kfault_terminate();
}

#if KICKOS_KERNEL_STACKS
// From svc_trampoline, when the calling thread has no kernel block seated. Runs privileged
// in THREAD mode ON THE MSP, .Lsvc_nokstack having cleared CONTROL.SPSEL before the branch;
// `psp` is the thread's own, computed before that clear.
void kickos_armv7m_no_kernel_stack(uint32_t psp)
{
    kpanic_enter();
    ::kickos::kprintf("\n=== ARMV7M EXCEPTION (no kernel stack) ===\n");
#if KICKOS_PANIC_DUMP
    ::kickos::kprintf("  in svc_trampoline PSP=0x%x\n", static_cast<unsigned>(psp));
#else
    (void)psp;
#endif
    kfault_terminate();
}
#endif

// `frame` points at the hardware-stacked exception frame {r0,r1,r2,r3,r12,lr,pc,xPSR};
// `exc_return` is the EXC_RETURN in LR, whose bit 2 selects the pre-fault stack.
void kickos_armv7m_fault_report(uint32_t* frame, uint32_t exc_return)
{
    // HardFault_Handler reaches here by a plain `b`, so this function's own return IS the
    // exception return. Nothing may print above this: kpanic_enter's console reclaim is
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
    // MMFAR/BFAR are stale unless the matching CFSR VALID bit is set (MMARVALID = bit 7,
    // BFARVALID = bit 15).
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

// Picks the stacked frame (MSP vs PSP per EXC_RETURN bit 2) and passes it, with EXC_RETURN,
// to the C reporter. Naked so no prologue perturbs SP before it is read. The chip vector
// tables point HardFault/MemManage/BusFault/UsageFault all here.
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
// The system-handler priorities the BASEPRI crit section depends on, and the DWT cycle
// counter that backs arch_trace_now.
void kickos_armv7m_init(void)
{
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
