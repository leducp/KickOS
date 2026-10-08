// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ARMv7-M arch backend: the parts of the arch.h seam this profile does not share with
// ARMv6-M. Context switch and syscall trap are in switch.S; the NVIC line set, the
// default-IRQ entry and the two switch.S refusal reports are in arch_arm_common.cc. The chip
// layer (arch/arm/chip/*) supplies arch_init, arch_console_write, SystemCoreClock and the
// linker script naming the user-RAM region.

#include <kickos/arch/arch.h>
#include "ctx_redirect.h"
#include "../common/fault_resume.h"
#include <kickos/diag.h>
#include <kickos/units.h> // _s literal (== 1e9 ns)

#include "probe_catch.h"
#include "regs.h"
#include <kickos/arch/armv7m_fault_frame.h>
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
// The spawn stages its grant list on the caller's stack, and _SVC holds the list it was
// measured at (armv7m_trap_stack.h).
static_assert(KICKOS_MAX_SPAWN_GRANTS <= 9,
              "KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVC holds a spawn staging 9 grants on the "
              "caller's stack, and 10 measure past it: re-measure with "
              "tests/static/check_trap_redzone.sh and raise the figure first");
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
// The spawn stages its grant list on the kernel block, and _SVCK holds the list it was
// measured at, per posture (armv7m_trap_stack.h).
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
static_assert(KICKOS_MIN_STACK_SIZE
                  >= KICKOS_ARMV7M_TRAP_NEED_SVC + KICKOS_ARMV7M_TRAP_ENTRY_FRAME,
              "KICKOS_MIN_STACK_SIZE is below the armv7m syscall red zone plus the "
              "exception frame entry spends above it: raise the per-arch default in "
              "Kconfig, never the red zone, which is a measurement");

// The reporter's own array (kernel/init/console.cc). The frame term is the hardware frame a
// HardFault stacks there, PRIMASK not masking one, so the two macros must agree.
static_assert(KICKOS_ARMV7M_PANIC_FRAME == KICKOS_ARMV7M_TRAP_FRAME_MAX,
              "the panic frame term is the hardware frame, which switch.S cannot compute");
static_assert(KICKOS_PANIC_STACK_SIZE >= KICKOS_ARMV7M_PANIC_FRAME + KICKOS_ARMV7M_PANIC_DEPTH,
              "KICKOS_PANIC_STACK_SIZE is below what this arch's panic reporter descends");

// AAPCS keeps SP 8-byte aligned at every public interface, so a kernel stack whose SIZE is
// not a multiple of 8 puts its top off that boundary.
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
static_assert(kickos::armv7m::PRIO_LOCK_BASEPRI == 0x20,
              "SVC_Handler's fastpath raises this level as a literal");

namespace
{
    using namespace kickos::arm;    // reg32 (shared core regs)
    using namespace kickos::armv7m; // PRIO_LOCK_BASEPRI, DWT_*, SHPR (arch-specific)
}

extern "C"
{
    // An unprivileged thread whose entry returns cannot run kickos_thread_return: with
    // nPRIV=1 the BASEPRI write in IrqLock is a no-op and the SCS write in arch_switch
    // BusFaults. This one traps out via the exit syscall.
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

    // ctx->kernel_sp IS DELIBERATELY UNTOUCHED. thread_create seats it BEFORE this call and
    // is the only writer of the zero that means no block seated, which svc_trampoline's
    // refusal path keys on; clearing it here would wipe the block off every fresh thread.
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

// --- Monotonic clock ---------------------------------------------------------
// arch_clock_now is a REQUIRED chip contract, over a dedicated peripheral timer. The DWT is
// debug-domain (gated by DEMCR.TRCENA, lockable on Cortex-M7, absent under QEMU), so this
// arch supplies no fallback and a board omitting its definition fails at link time rather
// than hanging on its first sleep. The one-shot SysTick timer is core-generic
// (arch_arm_common).

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
    // A stack overflow arrives as a stacking abort.
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

#if defined(KICKOS_ENABLE_SELFTEST) && KICKOS_AMP_OWN_IMAGE
// Its first instruction is the load a fault on which kickos_armv7m_probe_caught resumes past.
uint32_t kickos_armv7m_probe_word(uintptr_t at);
__asm__(".pushsection .text.kickos_armv7m_probe_word,\"ax\",%progbits\n"
        ".global kickos_armv7m_probe_word\n"
        ".type kickos_armv7m_probe_word, %function\n"
        ".thumb_func\n"
        "kickos_armv7m_probe_word:\n"
        "    ldr.n r0, [r0]\n"
        "    bx lr\n"
        ".size kickos_armv7m_probe_word, . - kickos_armv7m_probe_word\n"
        ".popsection\n");

static kickos::armv7m::ProbeLatch g_probe = {};

static bool kickos_armv7m_probe_caught(uint32_t* frame, uint32_t exc_return)
{
    uint32_t const load = reinterpret_cast<uint32_t>(&kickos_armv7m_probe_word) & ~1u;
    uint32_t control;
    __asm volatile("mrs %0, control" : "=r"(control));
    uint32_t const cfsr = kickos::arm::reg32(SCB_CFSR);
    if (not kickos::armv7m::probe_catch(g_probe, frame, load, exc_return, control, cfsr,
                                        kickos::arm::reg32(SCB_BFAR)))
    {
        return false;
    }
    // Write-1-to-clear, as on every other fault path.
    kickos::arm::reg32(SCB_CFSR) = cfsr;
    kickos::arm::reg32(SCB_HFSR) = kickos::arm::reg32(SCB_HFSR);
    return true;
}

// 0 with the word in `*value`, or the CFSR of the fault the read took with its BFAR, zero where
// the fault left none valid, in `*value`.
uint32_t kickos_armv7m_probe_read(uintptr_t at, uint32_t* value)
{
    return kickos::armv7m::probe_read(g_probe, kickos_armv7m_probe_word, at, value);
}
#endif

// `frame` points at the hardware-stacked exception frame {r0,r1,r2,r3,r12,lr,pc,xPSR};
// `exc_return` is the EXC_RETURN in LR, whose bit 2 selects the pre-fault stack.
void kickos_armv7m_fault_report(uint32_t* frame, uint32_t exc_return)
{
    // HardFault_Handler reaches here by a plain `b`, so this function's own return IS the
    // exception return. Nothing may print above this: kpanic_enter's console reclaim is
    // permanent and this fault is survivable.
    bool const frame_read = armv7m_fault_frame_readable(kickos::arm::reg32(SCB_CFSR));
#if defined(KICKOS_ENABLE_SELFTEST) && KICKOS_AMP_OWN_IMAGE
    if (frame_read and kickos_armv7m_probe_caught(frame, exc_return))
    {
        return;
    }
#endif
    if (kickos_fault_kill_thread(frame))
    {
        return;
    }
    kpanic_enter(); // mask IRQs + force the sync path + flush queued bytes, in order
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

// --- One-time core bring-up ------------------------------------------------
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
