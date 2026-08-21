// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/arch/arch.h>
#include <kickos/arch/rx_trap_stack.h> // the USP guards' derived figures + ctx offsets
#include <kickos/units.h> // _s literal (== 1e9 ns) for the cycle<->ns conversions

#include "regs.h"
#include <kickos/console_tx.h> // console_tx_isr: drained by the TXI ISR below
#include <kickos/sys/atomic.h>
#include <kickos/trace/record.h> // ArchId: pin this build's trace-arch id to this backend


#include <stddef.h> // offsetof

static_assert(KICKOS_TRACE_ARCH == kickos::trace::ARCH_RX,
              "KICKOS_TRACE_ARCH does not match ArchId::ARCH_RX for rxv3");

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

#if defined(__RX_DFPU_INSNS__)
static_assert(sizeof(double) == 8, "-mdfpu should give 64-bit doubles");
#endif

// switch.S hard-codes each offset below as a literal displacement and takes it from
// rx_trap_stack.h. Nothing in the language ties the two together, so a field inserted
// ahead of the bounds leaves a USP guard comparing against trace_tid, and that still
// assembles and still links: these assertions are what holds them together.
static_assert(offsetof(struct arch_context, sp) == KICKOS_RX_CTX_OFF_SP,
              "switch.S expects ctx.sp @0");
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
// The SWINT switcher reads the trace id at 4[ctx] (the `4[r15]` loads in switch.S).
static_assert(offsetof(struct arch_context, trace_tid) == KICKOS_RX_CTX_OFF_TRACE_TID,
              "switch.S expects trace_tid @4");
#endif
static_assert(offsetof(struct arch_context, stack_lo) == KICKOS_RX_CTX_OFF_STACK_LO,
              "switch.S reads stack_lo at F_CTX_STACK_LO");
static_assert(offsetof(struct arch_context, stack_hi) == KICKOS_RX_CTX_OFF_STACK_HI,
              "switch.S reads stack_hi at F_CTX_STACK_HI");
static_assert(sizeof(struct arch_context) >= KICKOS_RX_CTX_OFF_STACK_HI + sizeof(uint32_t),
              "a guard reads a word past the end of struct arch_context");

// The syscall guard runs above the arm selection, so its one figure has to dominate
// both arms.
static_assert(KICKOS_RX_TRAP_REDZONE_SYS
                  >= KICKOS_RX_TRAP_FRAME_SYS + KICKOS_RX_TRAP_KERNEL_DEPTH_SYS,
              "the syscall red zone does not cover the generic arm");
static_assert(KICKOS_RX_TRAP_REDZONE_SYS
                  >= KICKOS_RX_TRAP_FRAME_SYS_FAST + KICKOS_RX_TRAP_KERNEL_DEPTH_SYS_FAST,
              "the syscall red zone does not cover the fastpath arm");
// It must also carry a whole PENDSW zone, a SWINT pended over a running dispatch building
// its save on the same USP below it (rx_trap_stack.h derives the composition).
static_assert(KICKOS_RX_TRAP_REDZONE_SYS
                  >= KICKOS_RX_TRAP_FRAME_SYS + KICKOS_RX_TRAP_KERNEL_DEPTH_SYS
                         + KICKOS_RX_TRAP_REDZONE_PENDSW,
              "the syscall red zone does not cover a SWINT arriving mid-dispatch");

// The floor must DOMINATE the red zone, or a thread spawned at the floor passes the spawn
// check and is then refused by the guard on every syscall it makes. This arch had no such
// assertion, so the relation held only where check_trap_redzone.sh ran, and the RX reaches
// no hosted CI. Unlike the two ARM backends this needs no entry-frame term: RX accepts
// interrupts on the ISP, so nothing is spent above the USP the guard validates.
static_assert(KICKOS_MIN_STACK_SIZE >= KICKOS_RX_TRAP_REDZONE_SYS,
              "KICKOS_MIN_STACK_SIZE is below the rxv3 syscall red zone: raise the "
              "per-arch default in Kconfig, never the red zone, which is a measurement");

// arch_irq_save raises PSW.IPL to the lock level with an MVTIPL immediate; keep
// the literal in the asm string in sync with the constant.
static_assert(kickos::rxv3::IPL_LOCK == 12, "arch_irq_save MVTIPL literal is #12");

namespace
{
    using namespace kickos::rxv3;

    using kickos::Atomic;
    using kickos::Order;

    // Free-running CMTW1 is 32-bit; extend to a monotonic 64-bit count in software by
    // catching wraps on each read, so a wrap not observed within one 2^32-cycle period is
    // missed. The two words are ONE value, kept coherent by the IrqLock in now_cycles.
    uint32_t g_cyc_high = 0;
    uint32_t g_cyc_last = 0;

    // In-ISR nesting is tracked in software on RXv3. Bumped only by the first-level
    // DEVICE-IRQ dispatchers, never by the syscall INT path, so arch_in_isr() reads false
    // throughout syscall_dispatch (arch.h contract).
    Atomic<int, Order::RELAXED> g_in_isr = 0;

    // Software IRQ controller for INJECTED logical lines. Only the ICU's two software
    // interrupts are settable from software, so injected lines are delivered over the
    // single SWINT2 doorbell: g_inject_line carries the logical line, g_irq_masked gates
    // it. Lines < SOFT_IRQ_LINES are software (this controller); lines >= it are real ICU
    // vectors gated by ICU.IER (e.g. console TXI6 = 87). RX's own sub-32 vectors (SWINT
    // 26/27, timer CMWI0 30) are configured directly by the arch/chip init and never pass
    // through the arch_irq_* seam, so they do not collide. Masked-by-default: a line is
    // armed only by arch_irq_unmask (kernel irq_claim/irq_ack).
    constexpr int SOFT_IRQ_LINES = 32;
    uint32_t g_irq_masked = 0xFFFFFFFFu;
    Atomic<int, Order::RELAXED> g_inject_line = -1;
    // bit set = a raise landed on this soft line while masked (latched one-deep,
    // coalesced). Redelivered through the SWINT2 doorbell at unmask.
    uint32_t g_irq_pending = 0;

#ifndef KICKOS_RX_MPU_TRACE
#define KICKOS_RX_MPU_TRACE 0
#endif
#if KICKOS_RX_MPU_TRACE
    void rx_mpu_mark(char c)
    {
        constexpr uintptr_t SCI6_SSR = 0x0008A0C4; // TDRE = b7
        constexpr uintptr_t SCI6_TDR = 0x0008A0C3;
        uint32_t spin = 0;
        while ((reg8(SCI6_SSR) & (1u << 7)) == 0)
        {
            if (++spin > 200000u)
            {
                return; // wedged FIFO must never block the localizer itself
            }
        }
        reg8(SCI6_TDR) = static_cast<uint8_t>(c);
    }
#else
    inline void rx_mpu_mark(char) {}
#endif
}

extern "C"
{
    // Userspace thread epilogue: a user thread whose entry returns must trap out
    // via the exit syscall (it cannot run the kernel epilogue with PM=1). Seeded
    // by arch_context_init as the fabricated frame's return address for a user
    // thread (kickos_thread_return for a kernel thread).
    void kickos_user_thread_return(void);

    // The CMTW input-clock frequency in Hz (PCLKB / prescale), defined by the
    // chip. Drives the ns<->cycle conversions for the clock + one-shot timer.
    extern uint32_t kickos_rx_timer_hz;

    // Shared with switch.S: written by C and by asm.
    kickos::Atomic<struct arch_context*, kickos::Order::RELAXED> g_arch_current = nullptr;
    kickos::Atomic<struct arch_context*, kickos::Order::RELAXED> g_arch_next = nullptr;

    // switch.S loads each as a plain word at offset 0. Nothing else enforces the layout.
    static_assert(sizeof(g_arch_current) == sizeof(struct arch_context*), "asm reads one word");
    static_assert(sizeof(g_arch_next) == sizeof(struct arch_context*), "asm reads one word");
    static_assert(alignof(decltype(g_arch_current)) == alignof(struct arch_context*), "asm reads it naturally aligned");

    // CMSIS core clock (ICLK), defined + maintained by the chip at PLL lock.
    extern uint32_t SystemCoreClock;
    uint32_t arch_cpu_clock_hz(void)
    {
        return SystemCoreClock;
    }
}

namespace
{
    using namespace kickos::units; // _s == 1e9 ns

    inline uint64_t now_cycles()
    {
        // The wrap-extend read must be atomic against a concurrent reader; run it
        // under the crit section (only ever reached from privileged context).
        arch_irq_state_t s = arch_irq_save();
        uint32_t cur = reg32(CMTW1_BASE + CMTW_CMWCNT);
        if (cur < g_cyc_last)
        {
            ++g_cyc_high;
        }
        g_cyc_last = cur;
        uint64_t hi = g_cyc_high;
        arch_irq_restore(s);
        return (hi << 32) | cur;
    }

    inline uint64_t cycles_to_ns(uint64_t cyc)
    {
        uint64_t f = kickos_rx_timer_hz;
        if (f == 0)
        {
            return 0;
        }
        uint64_t sec = cyc / f;
        uint64_t rem = cyc % f;
        return sec * 1_s + (rem * 1_s) / f;
    }

    inline uint64_t ns_to_cycles(uint64_t ns)
    {
        uint64_t f = kickos_rx_timer_hz;
        return (ns * f) / 1_s;
    }

    inline void icu_ier_set(int line, bool on)
    {
        unsigned l = static_cast<unsigned>(line);
        volatile uint8_t& ier = reg8(ICU_IER_BASE + (l >> 3));
        uint8_t bit = static_cast<uint8_t>(1u << (l & 7));
        // IER packs 8 lines per byte, so this RMW must be atomic against a device
        // ISR's arch_irq_mask on a sibling line, and is callable from unlocked syscall
        // context (irq_attach/irq_unmask).
        arch_irq_state_t s = arch_irq_save();
        if (on)
        {
            ier = static_cast<uint8_t>(ier | bit);
        }
        else
        {
            ier = static_cast<uint8_t>(ier & ~bit);
        }
        arch_irq_restore(s);
    }

    // RX72M ICU IPR index for a vector. IR (UM sec.15.2.1) and IER (sec.15.2.2) are
    // indexed 1:1 by vector; IPR (sec.15.2.4) is shared, several sources collapsing onto
    // one entry, so the IPR index is NOT the vector number in general. This table carries
    // the mappings the UM interrupt vector table (sec.15.3.1) gives; an unlisted vector
    // falls back to identity, which holds for the 1:1 SCIg/peripheral block this backend
    // arms (SCI6 TXI6 = vector 87 -> IPR087). A shared source reaching identity would be
    // given the wrong IPR: list it, and cite the UM, before enabling that device line.
    struct ipr_map_entry
    {
        uint16_t vector;
        uint8_t ipr;
    };
    constexpr ipr_map_entry IPR_MAP[] = {
        {SWINT2_VECTOR, 3}, // SWINT2(26)+SWINT(27) share ICU.IPR[3] (RX72x BSP)
        {SWINT_VECTOR, 3},
        {CMWI0_VECTOR, 6},  // CMTW0 CMWI0(30) -> ICU.IPR[6]
    };

    inline unsigned vector_to_ipr(int vector)
    {
        for (unsigned i = 0; i < sizeof(IPR_MAP) / sizeof(IPR_MAP[0]); i++)
        {
            if (IPR_MAP[i].vector == vector)
            {
                return IPR_MAP[i].ipr;
            }
        }
        return static_cast<unsigned>(vector); // identity: 1:1 IPR sources
    }
}

// ===========================================================================
extern "C"
{

// Anything file-local below is `static`, never an anonymous namespace: C language
// linkage overrides the namespace, so an anonymous namespace nested in this block
// emits an unmangled GLOBAL symbol.

// --- Context init: fabricate a full switch-in frame (see switch.S layout) ---
// The frame is identical to what the SWINT switcher saves, so the first switch-in
// (arch_start) restores it and RTEs into entry(arg). Low->high on the USP:
//   [+0 A0LO][+4 A0HI][+8 A0GU][+12 A1LO][+16 A1HI][+20 A1GU][+24 FPSW]
//   [+28 R1=arg .. +84 R15][+88 PC=entry][+92 PSW]
// and, ABOVE the frame, [+96] = the address a returning entry() RTSes into
// (kickos_thread_return for a kernel thread, kickos_user_thread_return for a user one).
// RTE pops PC then PSW, delivering R1=arg per the psABI.
void arch_context_init(struct arch_context* ctx,
                       void (*entry)(void*), void* arg,
                       void* stack_base, size_t stack_size,
                       int privileged)
{
    constexpr uint32_t FPSW_INIT = 0x00000100u; // RX FPSW reset posture (RM sec.2.12)

    uintptr_t top = reinterpret_cast<uintptr_t>(stack_base) + stack_size;
    top &= ~static_cast<uintptr_t>(3); // 4-byte aligned stack
    uint32_t* sp = reinterpret_cast<uint32_t*>(top);

    uint32_t psw = PSW_THREAD_KERNEL;
    uint32_t ret = reinterpret_cast<uint32_t>(kickos_thread_return);
    if (not privileged)
    {
        psw = PSW_THREAD_USER;
        ret = reinterpret_cast<uint32_t>(kickos_user_thread_return);
    }

    *(--sp) = ret;                               // [+96] entry's eventual RTS target
    *(--sp) = psw;                               // [+92] PSW (RTE pops)
    *(--sp) = reinterpret_cast<uint32_t>(entry); // [+88] PC  (RTE pops)
    for (int i = 0; i < 14; i++)                 // [+84..+32] R15..R2 = 0
    {
        *(--sp) = 0;
    }
    *(--sp) = reinterpret_cast<uint32_t>(arg);   // [+28] R1 = arg (first C argument)
    *(--sp) = FPSW_INIT;                         // [+24] FPSW
    *(--sp) = 0;                                 // [+20] A1 guard
    *(--sp) = 0;                                 // [+16] A1 high
    *(--sp) = 0;                                 // [+12] A1 low
    *(--sp) = 0;                                 // A0 guard
    *(--sp) = 0;                                 // A0 high
    *(--sp) = 0;                                 // A0 low
#if defined(__RX_DFPU_INSNS__)
    // DPFPU register file, banked below the accumulators (switch.S DPUSHM/DPOPM). The
    // control words take the DPFPU reset posture (DPSW round-to-nearest, DECNT=1) per the
    // RXv3 ISA UM DPFPU reset values. Layout must mirror DPUSHM.D dr0-dr15 (higher) +
    // DPUSHM.L dpsw-decnt (lower): DR block first, then DECNT, DCMR, DPSW with DPSW
    // lowest = the new ctx.sp.
    for (int i = 0; i < 16 * 2; i++)             // DR0-DR15 (16 doubles, 2 words each)
    {
        *(--sp) = 0;
    }
    *(--sp) = 1;                                 // DECNT (reset value 1)
    *(--sp) = 0;                                 // DCMR
    *(--sp) = 0x00000100u;                       // DPSW (lowest word)
#endif

    ctx->sp = reinterpret_cast<uint32_t>(sp);

    // The syscall trap and SWINT switcher validate the live USP against these before they
    // store a frame through it. `top` is the 4-byte-aligned high edge, so a running
    // thread's USP stays in [stack_lo, stack_hi].
    ctx->stack_lo = reinterpret_cast<uint32_t>(stack_base);
    ctx->stack_hi = static_cast<uint32_t>(top);
}

#if defined(KICKOS_ARCH_HAS_IPC_FASTPATH) && KICKOS_ARCH_HAS_IPC_FASTPATH
// The fastpath parks a caller on the frame the trap built for it, so the result has to
// be seated where the restore reloads R1 from. R1 is the register the RX psABI answers
// in, and its slot sits above the DPFPU bank and the accumulators; switch.S spells the
// same offset as FRAME_R1_OFF.
#if defined(__RX_DFPU_INSNS__)
constexpr uint32_t FRAME_R1_OFF = 168;
#else
constexpr uint32_t FRAME_R1_OFF = 28;
#endif

void arch_ctx_set_syscall_result(struct arch_context* ctx, uint32_t result)
{
    reinterpret_cast<uint32_t*>(ctx->sp + FRAME_R1_OFF)[0] = result;
}
#endif

// The whole seam on this backend: the fabricated frame's PSW word is PSW_THREAD_KERNEL,
// which the RTE pops. The MPU_MPECLR latch arch_fault_redirect_to_exit clears is global
// and belongs to a fault, so a rebuild must not touch it.
void arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                       void* stack_base, size_t stack_size)
{
    arch_context_init(ctx, entry, nullptr, stack_base, stack_size, 1);
}

// --- Critical section: raise PSW.IPL to the kernel lock level ---------------
arch_irq_state_t arch_irq_save(void)
{
    uint32_t psw;
    __asm volatile("mvfc psw, %0" : "=r"(psw));
    // Raise to IPL_LOCK (12). MVTIPL takes an immediate only, so the level is a
    // literal (static_assert above pins it), and it is self-synchronizing.
    __asm volatile("mvtipl #12" ::: "memory");
    return (psw & PSW_IPL_MASK) >> PSW_IPL_SHIFT; // old IPL
}

void arch_irq_restore(arch_irq_state_t state)
{
    // Restore only the IPL field: MVTIPL cannot take a runtime value, so the whole PSW
    // goes back via MVTC with just the IPL bits replaced. PM is ignored on write in
    // supervisor; flags/U/I are preserved from the current PSW.
    uint32_t psw;
    __asm volatile("mvfc psw, %0" : "=r"(psw));
    psw = (psw & ~PSW_IPL_MASK) |
          ((static_cast<uint32_t>(state) << PSW_IPL_SHIFT) & PSW_IPL_MASK);
    __asm volatile("mvtc %0, psw" ::"r"(psw) : "memory");
}

int arch_in_isr(void)
{
    return g_in_isr;
}

// --- Fault isolation --------------------------------------------------------
// What the core hands back to the two seams. The saved pair is [0]=PC, [4]=PSW on the
// ISP: exception pre-processing writes both to the stack ISP selects, never to the USP
// (RXv3 ISA UM sec.5.2 + Table 5.2), and RTE pops PC first then PSW (sec.3, RTE).
// `cause` rides along: the fixed-vector offset is known only to the shim that took the
// vector.
struct RxFaultFrame
{
    uint32_t* saved;
    uint32_t cause;
};

// The five instruction-CANCELING exceptions, the ones whose saved PC is the instruction
// that generated them (ISA UM sec.5.3.1 Table 5.1). Cause 0 is the _rx_trap catch-all,
// which carries the NMI and BRK: an NMI is accepted at an instruction boundary with
// PSW.PM still set, so admitting it here would kill whichever thread happened to be
// running for a chip-level event.
static bool rx_cause_is_thread_fault(uint32_t cause)
{
    return cause == 0x50 or cause == 0x54 or cause == 0x5C or cause == 0x60
           or cause == 0x64;
}

bool arch_fault_is_user_thread(void* frame)
{
    RxFaultFrame const* const ff = static_cast<RxFaultFrame const*>(frame);
    // PSW.PM is the mode BEFORE the exception, and the copy carrying it sits on the ISP,
    // which no user thread can reach, so the frame itself is trusted as read.
    if ((ff->saved[1] & PSW_PM) == 0)
    {
        return false;
    }
    if (not rx_cause_is_thread_fault(ff->cause))
    {
        return false;
    }
    // RTE restores U=1 and the stub executes on the thread's USP, exactly where
    // svc_trampoline runs, so the USP is what must lie in the thread's own stack: a
    // thread that wrecked R0 would otherwise put privileged code on a stack of its
    // choosing. Length 0 asks for containment alone: the stack grows DOWN from here and
    // no frame has been written at this address.
    uint32_t usp;
    __asm volatile("mvfc usp, %0" : "=r"(usp));
    if (not kickos_fault_frame_trusted(reinterpret_cast<void*>(usp), 0))
    {
        return false;
    }
#if KICKOS_HAVE_MPU
    // A STACK OVERFLOW reads as in-bounds in the USP: every one of these exceptions
    // CANCELS its instruction and restores SP (ISA UM sec.5.3.1), so the denied push
    // leaves the USP where it was and the containment test above passes with nothing left
    // below. MPDEA is the only register that says so. Supervisor bypasses the RX MPU, so
    // a stub run on the exhausted stack smashes its way out untrapped.
    if (ff->cause == 0x54 and (reg32(MPU_MPESTS) & MPU_MPESTS_DMPER) != 0)
    {
        if (kickos_fault_below_stack(reg32(MPU_MPDEA)))
        {
            return false;
        }
    }
#endif
    return true;
}

void arch_fault_redirect_to_exit(void* frame)
{
    RxFaultFrame* const ff = static_cast<RxFaultFrame*>(frame);
    char const* status_name = "cause";
    uint32_t status = ff->cause;
    uintptr_t addr = 0;
    int addr_valid = 0;
#if KICKOS_HAVE_MPU
    if (ff->cause == 0x54)
    {
        // MPESTS carries IMPER/DMPER/DRW, so it says more here than the cause does.
        status_name = "MPESTS";
        status = reg32(MPU_MPESTS);
        if ((status & MPU_MPESTS_DMPER) != 0)
        {
            addr = reg32(MPU_MPDEA);
            addr_valid = 1;
        }
        else if ((status & MPU_MPESTS_IMPER) != 0)
        {
            addr = ff->saved[0]; // instruction fetch: the address IS the saved PC
            addr_valid = 1;
        }
        // Latched, and cleared only through MPECLR. This fault is survivable, so a bit
        // left set would label the NEXT thread's fault with it.
        reg32(MPU_MPECLR) = MPU_MPECLR_CLR;
    }
#endif
    kickos_fault_record(status_name, status, ff->saved[0], addr, addr_valid);

    // The syscall trap's own rewrite (switch.S kickos_rx_syscall_trap), for the same
    // posture: supervisor-on-USP, i.e. privileged in thread mode on the thread's own
    // stack. U is WRITTEN rather than inherited because RTE forces it only on a
    // transition to user mode and this transition is the other way. I=1 with IPL=0 is
    // what lets the stub's exit_current reschedule: it pends SWINT and must see it taken.
    ff->saved[0] = reinterpret_cast<uint32_t>(&kickos_thread_fault_exit);
    ff->saved[1] = (ff->saved[1] & ~(PSW_PM | PSW_IPL_MASK)) | PSW_U | PSW_I;

    // The stub runs at the TOP of the dying thread's stack, not at the depth the fault
    // reached: an access exception restores SP (ISA UM sec.5.3.1), so an overflowed thread
    // hands over a USP that reads in-bounds and the stub would otherwise run privileged on
    // an exhausted stack. Safe to write here because the handler runs on the ISP: R0 in
    // supervisor mode is the ISP, and USP is a separate control register.
    uint32_t const top = static_cast<uint32_t>(kickos_fault_stack_top());
    if (top != 0)
    {
        uint32_t const sp = top & ~3u;
        __asm volatile("mvtc %0, usp" ::"r"(sp));
    }
}

// The syscall trap and SWINT switcher (switch.S) call this when the live USP is outside
// the running thread's stack: R0 is the USP in user mode, so a wild USP would run
// privileged dispatch on a caller-chosen stack. Runs on the ISP (trusted). Panics rather
// than killing the thread, because containment needs a frame worth trusting and a safe
// USP for the exit stub, and a refused USP supplies neither.
void kickos_rx_bad_usp(uint32_t usp)
{
    kpanic_enter();
#if defined(KICKOS_ENABLE_SELFTEST)
    kickos_trapstack_witness_report();
#endif
    ::kickos::kprintf("\n=== RX EXCEPTION (wild stack) ===\n  USP=0x%x\n",
                      static_cast<unsigned>(usp));
    kfault_terminate();
}

// C side of the RX exception handler (startup.S .fvectors shims branch here with
// r1=cause [the fixed-vector offset], r2=the saved PC/PSW pair on the ISP). Runs on the
// ISP in supervisor mode. kpanic_enter masks IRQs (raises PSW.IPL, never restored; this
// path does not return), forces the polled writer, and flushes the ring.
//
// RETURNING means the fault was redirected: the shim's RTE then lands in
// kickos_thread_fault_exit instead of resuming the faulting instruction.
void kickos_rx_fault_report(uint32_t cause, uint32_t* saved)
{
    // Nothing may run above this, not even the raw localizer below: kpanic_enter's
    // console reclaim is permanent and this fault is survivable.
    RxFaultFrame ff = {saved, cause};
    if (kickos_fault_kill_thread(&ff))
    {
        return;
    }
    uint32_t const pc = saved[0];
    uint32_t const psw = saved[1];
    rx_mpu_mark('F'); // localizer: an exception fired (a FAULT, not a hang), raw, pre-console
    kpanic_enter();
#if defined(KICKOS_ENABLE_SELFTEST)
    kickos_trapstack_witness_report(); // names a U-mode USP that reached kernel memory
#endif
#if KICKOS_HAVE_MPU
    // The access exception (fixed vector +0x54) IS the RX MPU violation, and the RX MPU
    // checks user mode only (UM sec.17.1.1), so one taken with the faulting PSW.PM set is
    // an unprivileged thread hitting an ungranted region. MPESTS.DMPER => operand access
    // (address in MPDEA, DRW gives read vs write); MPESTS.IMPER => instruction fetch
    // (address is the stacked PC). An access exception from supervisor cannot be an MPU
    // fault (supervisor is never checked), so it falls through to the generic dump = a
    // genuine kernel bug.
    if (cause == 0x54 and (psw & PSW_PM) != 0)
    {
        uint32_t const sts = reg32(MPU_MPESTS);
        if ((sts & (MPU_MPESTS_IMPER | MPU_MPESTS_DMPER)) != 0)
        {
            uintptr_t addr = pc;
            int is_write = 0;
            if ((sts & MPU_MPESTS_DMPER) != 0)
            {
                addr = reg32(MPU_MPDEA);
                if ((sts & MPU_MPESTS_DRW) != 0)
                {
                    is_write = 1;
                }
            }
            reg32(MPU_MPECLR) = MPU_MPECLR_CLR;
            kickos_isr_fault(addr, is_write); // names the task, then arch_shutdown (noreturn)
        }
    }
#endif
    char const* what = "trap";
    if (cause == 0x50)
    {
        what = "privileged instruction";
    }
    else if (cause == 0x54)
    {
        what = "access exception";
    }
    else if (cause == 0x5C)
    {
        what = "undefined instruction";
    }
    else if (cause == 0x60)
    {
        what = "address exception";
    }
    else if (cause == 0x64)
    {
        what = "floating-point";
    }
#if KICKOS_PANIC_DUMP
    ::kickos::kprintf("\n=== RX EXCEPTION (%s) ===\n  PC=0x%x PSW=0x%x\n", what, pc, psw);
#else
    (void)pc;
    (void)psw;
    ::kickos::kprintf("\n=== RX EXCEPTION (%s) ===\n", what);
#endif
    kfault_terminate();
}

// --- Tickless clock (CMTW1) + one-shot timer (CMTW0) ------------------------
uint64_t arch_clock_now(void)
{
    return cycles_to_ns(now_cycles());
}

// --- Trace clock (telemetry timestamp seam) ---------------------------------
// The free-running 32-bit CMTW1 counter IS the raw trace clock: u32, wraps on its own,
// host reconstructs absolute time from the SESSION clock_hz anchors (arch.h). Same source
// as arch_clock_now, read raw. KICKOS_HAVE_TRACE_CLOCK is set for rxv3.
uint32_t arch_trace_now(void)
{
    return reg32(CMTW1_BASE + CMTW_CMWCNT);
}


// Last absolute deadline programmed into CMTW0 (UINT64_MAX == disarmed). Touched
// only from arch_timer_arm/disarm, which run under the kernel IrqLock, so it stays
// in sync with the hardware.
static uint64_t g_rx_armed_ns = ~0ull;

void arch_timer_arm(uint64_t deadline_ns)
{
    // Idempotent re-arm: ktime_rearm calls this on EVERY context switch. If the one-shot
    // is already running toward this exact deadline, leave CMWCNT alone; resetting it to 0
    // each switch means the compare is never reached, so a far deadline starves whenever
    // threads ping-pong faster than it. "Running toward it" must be tracked in software,
    // NOT read back from CMWSTR.STR, which races at full switch speed. The timer ISR sets
    // g_rx_armed_ns = ~0 before it re-arms, so its own re-arm is never skipped and a
    // reschedule with the same pending deadline is.
    if (deadline_ns == g_rx_armed_ns)
    {
        return;
    }
    g_rx_armed_ns = deadline_ns;
    uint64_t now = arch_clock_now();
    uint64_t delta_ns = 0;
    if (deadline_ns > now)
    {
        delta_ns = deadline_ns - now;
    }
    // Clamp the delta to the 32-bit one-shot range BEFORE converting so a
    // far-future deadline can't overflow ns*freq; a clamped deadline fires early
    // and the kernel re-arms the remainder (harmless extra wake).
    uint64_t f = kickos_rx_timer_hz;
    uint64_t max_delta_ns = ~0ull;
    if (f != 0)
    {
        max_delta_ns = (static_cast<uint64_t>(0xFFFFFFFFu) * 1_s) / f;
    }
    uint64_t cyc;
    if (delta_ns >= max_delta_ns)
    {
        cyc = 0xFFFFFFFFu;
    }
    else
    {
        cyc = ns_to_cycles(delta_ns);
    }
    if (cyc == 0)
    {
        cyc = 1; // never program 0
    }
    // One-shot from zero: stop, clear counter, load compare, enable interrupt +
    // clear-on-match, start. CMWCR clock/prescale is set once by the chip init.
    reg16(CMTW0_BASE + CMTW_CMWSTR) = 0;
    reg32(CMTW0_BASE + CMTW_CMWCNT) = 0;
    reg32(CMTW0_BASE + CMTW_CMWCOR) = static_cast<uint32_t>(cyc);
    reg16(CMTW0_BASE + CMTW_CMWCR) = CMWCR_CKS_PCLK8 | CMWCR_CCLR_ON_MATCH | CMWCR_CMWIE;
    // Gate that actually arms the CMWCOR compare; without it the counter free-runs past
    // CMWCOR with no clear and no CMWI (UM sec.32.2.3). Reset value is 0, and it is
    // rewritten on each arm so no path can leave it clear.
    reg16(CMTW0_BASE + CMTW_CMWIOR) = CMWIOR_CMWE;
    reg16(CMTW0_BASE + CMTW_CMWSTR) = CMWSTR_STR;
}

void arch_timer_disarm(void)
{
    g_rx_armed_ns = ~0ull;
    reg16(CMTW0_BASE + CMTW_CMWSTR) = 0;
    reg8(ICU_IR_BASE + CMWI0_VECTOR) = 0; // drop a pending compare-match request
}

// --- MPU: per-thread memory protection (RX72M MPU, UM sec.17) ---------------
// On RX the MPU checks accesses ONLY in user mode; supervisor is never checked and always
// permitted (UM sec.17.1.1, the sec.17.3.4 flow). A PRIVILEGED (PM=0) thread keeps full
// access no matter what these registers hold, and there is no supervisor permission field
// at all. Enforcement therefore reduces to: a no-access background (MPBAC=0) so a user
// thread faults everywhere it has no explicit region, plus the running thread's regions
// loaded into the eight RSPAGEn/REPAGEn slots.
//
// Deferred-commit seam (docs/design-mpu-commit-deferred.md): arch_mpu_apply only STASHES
// the incoming set; kickos_arch_mpu_commit programs the RSPAGEn/REPAGEn slots from the
// SWINT switch epilogue (switch.S, kickos_rx_restore) AFTER the physical register/PSW
// swap. Eager apply on RX's deferred SWINT switch would load the incoming region set while
// the OUTGOING user thread is still physically running -> it faults on its own stack.
#if KICKOS_HAVE_MPU
// Pack the region set into the RSPAGEn/REPAGEn pair per slot. The page masks are field
// encoding, not rounding: a region the 16-byte pages cannot represent EXACTLY gets REPAGE
// 0 (V clear), never a window widened by up to 15 bytes on each side.
uint32_t arch_mpu_encode(struct arch_mpu_region const* regions, size_t n,
                         struct arch_mpu_encoded* out)
{
    if (n > ARCH_MPU_ENCODED_SLOTS)
    {
        n = ARCH_MPU_ENCODED_SLOTS;
    }
    uint32_t seated = 0;
    size_t i = 0;
    for (; i < n; i++)
    {
        out->rspage[i] = 0;
        out->repage[i] = 0;
        if (arch_mpu_region_encodable(regions[i].base, regions[i].size))
        {
            uintptr_t const base = regions[i].base;
            uintptr_t const end = base + regions[i].size - 1; // inclusive last byte
            uint32_t uac = 0;
            if (regions[i].attr & ARCH_MPU_R)
            {
                uac |= MPU_UAC_R;
            }
            if (regions[i].attr & ARCH_MPU_W)
            {
                uac |= MPU_UAC_W;
            }
            if (regions[i].attr & ARCH_MPU_X)
            {
                uac |= MPU_UAC_X;
            }
            out->rspage[i] = static_cast<uint32_t>(base) & MPU_PAGE_MASK;
            out->repage[i] =
                (static_cast<uint32_t>(end) & MPU_PAGE_MASK) | uac | MPU_REPAGE_V;
            seated |= static_cast<uint32_t>(1) << i;
        }
    }
    for (; i < ARCH_MPU_ENCODED_SLOTS; i++)
    {
        out->rspage[i] = 0;
        out->repage[i] = 0;
    }
    return seated;
}

// The raw set travels beside the image because the same-set skip below compares region
// extents: an image POINTER cannot answer that question, since a self-grant re-encodes in
// place and leaves the pointer unchanged.
static struct arch_mpu_region const* g_pend_regions = nullptr;
static size_t g_pend_count = 0;
static struct arch_mpu_encoded const* g_pend_image = nullptr;

void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n,
                    struct arch_mpu_encoded const* image)
{
    if (n > MPU_REGION_COUNT)
    {
        n = MPU_REGION_COUNT;
    }
    g_pend_regions = regions;
    g_pend_count = n;
    g_pend_image = image;
}

// Program the RX MPU from the stash. Called from the SWINT switcher (kickos_rx_pendsw
// -> kickos_rx_restore, and the arch_start first-entry path) after the physical swap.
// That handler already runs with PSW.I=0; the arch_irq_save/restore bracket keeps the seam
// callable from anywhere else.
void kickos_arch_mpu_commit(void)
{
    arch_irq_state_t const irq = arch_irq_save();
    struct arch_mpu_region const* const regions = g_pend_regions;
    size_t const n = g_pend_count;
    struct arch_mpu_encoded const* const img = g_pend_image;
    if (img == nullptr)
    {
        arch_irq_restore(irq);
        return;
    }
    if (g_in_isr)
    {
        rx_mpu_mark('['); // localizer: entering the MPU register writes from ISR ctx
    }
    // One-time: background = no user access (UBAC=0), then enable. Overlaps OR their
    // permission bits with the background (UM sec.17.1.4), so MPBAC must stay 0 or every
    // user thread is silently granted. MPOPI.INV clears any stale region V bits. MPU
    // registers are supervisor-only and not PRCR-gated (UM Table 13.1). MPEN takes effect
    // on the RTE into user mode (UM sec.17.2.3).
    static bool mpu_ready = false;
    if (not mpu_ready)
    {
        reg16(MPU_MPOPI) = MPU_MPOPI_INV;
        reg32(MPU_MPBAC) = 0;
        reg32(MPU_MPEN) = MPU_MPEN_MPEN;
        mpu_ready = true;
    }
    // Skip the register rewrite when the incoming set already matches the last applied
    // one: RR ping-pong between privileged threads would otherwise reprogram the identical
    // kernel-domain region from the timer ISR on every tick.
    static struct arch_mpu_region s_last[MPU_REGION_COUNT];
    static size_t s_last_n = ~static_cast<size_t>(0);
    bool same = (n == s_last_n);
    for (size_t i = 0; same and i < n and i < MPU_REGION_COUNT; i++)
    {
        if (s_last[i].base != regions[i].base or s_last[i].size != regions[i].size
            or s_last[i].attr != regions[i].attr)
        {
            same = false;
        }
    }
    if (not same)
    {
        // Write RSPAGEn (start) BEFORE REPAGEn, and put V in the REPAGEn write so a slot
        // is never momentarily valid with a stale end/attr.
        for (size_t i = 0; i < MPU_REGION_COUNT; i++)
        {
            uintptr_t const rsp = MPU_RSPAGE_BASE + i * MPU_REGION_STRIDE;
            uintptr_t const rep = MPU_REPAGE_BASE + i * MPU_REGION_STRIDE;
            if (i < ARCH_MPU_ENCODED_SLOTS and (img->repage[i] & MPU_REPAGE_V))
            {
                reg32(rsp) = img->rspage[i];
                reg32(rep) = img->repage[i];
            }
            else
            {
                reg32(rep) = 0; // clears V -> slot inactive
            }
        }
        // UM sec.17.4.3: read back an MPU register so the writes are in effect before the
        // scheduler's RTE drops into user mode. The asm consumes the value so the volatile
        // load is really issued and is not reordered past here.
        uint32_t const mpu_sync = reg32(MPU_MPEN);
        __asm volatile("" ::"r"(mpu_sync) : "memory");
        s_last_n = n;
        for (size_t i = 0; i < n and i < MPU_REGION_COUNT; i++)
        {
            s_last[i] = regions[i];
        }
    }
    if (g_in_isr)
    {
        rx_mpu_mark(']'); // localizer: MPU writes + readback barrier completed
    }
    arch_irq_restore(irq);
}
#else
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n,
                    struct arch_mpu_encoded const* image)
{
    (void)regions;
    (void)n;
    (void)image;
}
// The SWINT restore epilogue calls this unconditionally.
void kickos_arch_mpu_commit(void) {}
#endif

size_t arch_mpu_min_region(void)
{
    // RX MPU page = 16 bytes (RSPAGEn/REPAGEn address[31:4], UM sec.17.1.2). The
    // hardware takes arbitrary page-granular bounds, so this page size is also the
    // alloc/linker rounding granule.
    return 16u;
}

// The RX MPU is byte-granular on a 16-byte page (RSPAGEn/REPAGEn hold addr[31:4]);
// a window is exact iff base and base+size both land on a 16-byte boundary.
bool arch_mpu_region_encodable(uintptr_t base, size_t size)
{
    if (size < 16u)
    {
        return false;
    }
    return (base & 15u) == 0 and (size & 15u) == 0;
}

// The RSPAGEn/REPAGEn pair holds arbitrary 16-byte-page bounds, so a region size is free.
int arch_mpu_region_pow2(void)
{
    return 0;
}

// Every RX part in tree runs the arena uncached, and an RX MPU region pair encodes
// permissions only, so a nocache request is already satisfied.
int arch_mpu_nocache_support(void)
{
    return ARCH_MPU_NOCACHE_ALREADY;
}

// Rule 7 (arch.h): bit-band is a Cortex-M alias window, so RXv3 answers 0. An RX link
// resolves this seam here, so the definition is mandatory.
int arch_bitband_present(void)
{
    return 0;
}



// --- Interrupt controller (ICUD) --------------------------------------------
// Chip hooks, both lone-TU seams (arch/CMakeLists.txt states the rule).
//
// kickos_rx_dev_dispatch: called from the shared first-level ISR. The chip reads its own
// status registers and must call kickos_isr_irq ONCE PER ASSERTED SOURCE; a group vector
// can assert several at once (UM sec.15.5.4 Fig.15.17 p.542). It also owns the per-source
// clear, so the generic entry writes no IRn: an edge vector's IRn is already cleared by the
// ICU on acceptance, and a level group source's must not be written (UM sec.15.2.1 p.480).
//
// kickos_rx_group_arm: arm (on != 0) or disarm a GROUP-source logical line at its
// GENxxx.ENj. The group registers, the group -> vector map and the lazy arming of the group
// vector itself belong to the chip; the core owns only the line-space split.
void kickos_rx_dev_dispatch(void);
void kickos_rx_group_arm(int line, int on);

// Contract in rx_group.h: kickos_rx_group_arm arms a group's own VECTOR through here
// rather than back through arch_irq_unmask, a route the trap red-zone measurement reads
// as unbounded recursion. Caller holds arch_irq_save.
void kickos_rx_icu_line_arm(int line, int on)
{
    if (on != 0)
    {
        reg8(ICU_IPR_BASE + vector_to_ipr(line)) = static_cast<uint8_t>(IPL_DEVICE);
        icu_ier_set(line, true);
        return;
    }
    icu_ier_set(line, false);
}

// Self-bracketed (arch_irq_save/restore) so the soft-line g_irq_masked/g_irq_pending
// RMWs are atomic against a device ISR regardless of the caller: kos_irq_inject/unmask
// reach here without an IrqLock (syscall.cc), and a bare RMW preempted mid-update would
// write back a stale mask -> re-enable a mid-service line -> phantom wake.
void arch_irq_mask(int line)
{
    if (line < 0)
    {
        return;
    }
    arch_irq_state_t s = arch_irq_save();
    if (line < SOFT_IRQ_LINES)
    {
        g_irq_masked |= (1u << line);
    }
    else if (line >= GROUP_LINE_BASE)
    {
        kickos_rx_group_arm(line, 0);
    }
    else
    {
        icu_ier_set(line, false);
    }
    arch_irq_restore(s);
}

void arch_irq_unmask(int line)
{
    if (line < 0)
    {
        return;
    }
    arch_irq_state_t s = arch_irq_save();
    if (line < SOFT_IRQ_LINES)
    {
        g_irq_masked &= ~(1u << line);
        // Latch-and-coalesce: a raise taken on this soft line while masked
        // redelivers now through SWINT2: the normal ISR path, not a direct post.
        if ((g_irq_pending & (1u << static_cast<unsigned>(line))) != 0)
        {
            g_irq_pending &= ~(1u << line);
            g_inject_line = line;
            reg8(ICU_SWINT2R) = SWINT2R_SWINT2;
        }
    }
    else if (line >= GROUP_LINE_BASE)
    {
        // A group SOURCE's mask is GENxxx.ENj. The group VECTOR's own IPR/IER are armed
        // lazily by the chip once some source in that group is armed.
        kickos_rx_group_arm(line, 1);
    }
    else
    {
        // Program the source priority BELOW the kernel lock level before enabling, so a
        // device line cannot preempt an IrqLock-held section. IPR is shared per the ICU
        // source table, so the index comes from vector_to_ipr, NOT the vector (IR/IER stay
        // vector-indexed via icu_ier_set). A real ICU line is preserve-correct already: IR
        // latches a request while IER=0, so re-enabling here fires a raise taken while
        // masked.
        reg8(ICU_IPR_BASE + vector_to_ipr(line)) = static_cast<uint8_t>(IPL_DEVICE);
        icu_ier_set(line, true);
    }
    arch_irq_restore(s);
}

void arch_irq_clear_pending(int line)
{
    if (line < 0)
    {
        return;
    }
    arch_irq_state_t s = arch_irq_save();
    if (line < SOFT_IRQ_LINES)
    {
        g_irq_pending &= ~(1u << line);
    }
    else if (line >= GROUP_LINE_BASE)
    {
        // A group source is level-detected: GRPxxx.ISj and the group's IRn both FOLLOW
        // the source and go down only when the peripheral request clears or GENxxx.ENj is
        // written 0 (UM sec.15.5.4 p.542). GRPxxx is read-only, and sec.15.2.25 p.505
        // gives a clear register to the edge groups IE0/BE0 alone, so the clear belongs to
        // the DRIVER's own peripheral write (RULE L1). A no-op is the correct answer here:
        // the level-rearm path calls this before every unmask.
    }
    else
    {
        // Real ICU line: drop the latched request flag (IR is vector-indexed). Only an
        // EDGE vector may be written; see the ICU_IR_BASE note in regs.h.
        reg8(ICU_IR_BASE + static_cast<unsigned>(line)) = 0;
    }
    arch_irq_restore(s);
}

void arch_irq_inject(int irq)
{
    // Only logical lines are injectable: a real peripheral line cannot be pended from
    // software on RX, so anything >= SOFT_IRQ_LINES drops.
    if (irq < 0 or irq >= SOFT_IRQ_LINES)
    {
        return;
    }
    arch_irq_state_t s = arch_irq_save();
    // Latch-and-coalesce: a raise on a masked soft line latches one-deep, redelivered
    // at unmask.
    if ((g_irq_masked & (1u << static_cast<unsigned>(irq))) != 0)
    {
        g_irq_pending |= (1u << irq);
    }
    else
    {
        g_inject_line = irq; // recorded BEFORE the doorbell (the ISR reads it)
        reg8(ICU_SWINT2R) = SWINT2R_SWINT2; // ring SWINT2 -> kickos_rx_swint2 dispatches it
    }
    arch_irq_restore(s);
}

// --- Idle -------------------------------------------------------------------
void arch_idle_wait(void)
{
    __asm volatile("wait");
}

// --- First-level ISRs (C via __attribute__((interrupt))) --------------------
// GCC emits the full GPR save/RTE for these; the INTB table (chip startup) routes
// CMWI0 -> the timer ISR and every other line -> the default stub.
__attribute__((interrupt)) void kickos_rx_timer_isr(void)
{
    rx_mpu_mark('T'); // localizer: CMTW0 timer accepted (first RR-preempt path)
    reg8(ICU_IR_BASE + CMWI0_VECTOR) = 0; // clear the request flag
    reg16(CMTW0_BASE + CMTW_CMWSTR) = 0;  // one-shot: stop until re-armed
    g_rx_armed_ns = ~0ull;                // invalidate so kickos_isr_timer's re-arm reprograms
    g_in_isr = g_in_isr + 1;
    kickos_isr_timer(); // re-arms the next deadline
    g_in_isr = g_in_isr - 1;
}

// SCI6 transmit-data-empty (TXI6, vector 87), routed from INTB[87] by the chip's
// startup.S. TXI6 is edge-triggered (UM sec.15.3.1 Table 15.5 p.523): the ICU clears IR087
// on accept, so no source flag is touched here; console_tx_isr pushes bytes and gates
// SCR.TIE, which re-arms the edge.
//
// The INTB slot is fixed in flash, so this vector is SHARED between two owners over the
// life of the boot. Once kos_console_publish has run console_tx_deinit (handler detached,
// line masked, ring disarmed) a raise here can only belong to a driver that claimed vector
// 87; routing it to console_tx_isr would find an empty ring and call the backend's
// irq_disable, clearing SCR.TIE behind the driver's back.
__attribute__((interrupt)) void kickos_rx_console_txi_isr(void)
{
    g_in_isr = g_in_isr + 1;
    if (console_tx_armed() != 0)
    {
        console_tx_isr();
    }
    else
    {
        kickos_isr_irq(SCI6_TXI_VECTOR);
    }
    g_in_isr = g_in_isr - 1;
}

// SWINT2 doorbell: the software IRQ controller's delivery vector. arch_irq_inject
// latched the logical line in g_inject_line and pended SWINT2; run its bound handler.
// Clear IR026 first: a software interrupt's request flag survives acceptance, so leaving
// it set re-fires the doorbell forever. kickos_isr_irq runs the handler, which masks the
// line itself (irq_event_isr / the null-object default).
__attribute__((interrupt)) void kickos_rx_swint2(void)
{
    reg8(ICU_IR_BASE + SWINT2_VECTOR) = 0;
    g_in_isr = g_in_isr + 1;
    int line = g_inject_line;
    g_inject_line = -1;
    if (line >= 0)
    {
        kickos_isr_irq(line);
    }
    g_in_isr = g_in_isr - 1;
}

// Device-line default entry: the shared first-level ISR for every INTB device slot that
// has no dedicated one. RXv3 leaves the accepted vector unreadable from the handler, so
// the chip reads its OWN status registers and posts what it finds. The default
// kickos_rx_dev_dispatch does nothing, so this entry stays inert until a chip overrides it.
__attribute__((interrupt)) void kickos_rx_default_irq(void)
{
    g_in_isr = g_in_isr + 1;
    kickos_rx_dev_dispatch();
    g_in_isr = g_in_isr - 1;
}

// SCI6 receive-data-full (RXI6, vector 86), routed from INTB[86] by the chip's startup.S.
// It needs a DEDICATED slot rather than the shared dispatch above because an edge source
// cannot be discovered after the fact: the ICU clears IR086 when the request is accepted
// (UM sec.15.2.1(1) p.480), so any dispatch would find the flag it must test already 0.
// No flag is written here for the same reason: a write after the handler would discard a
// request latched while irq_event_isr had the line IER-masked, i.e. drop a received byte.
__attribute__((interrupt)) void kickos_rx_sci6_rxi_isr(void)
{
    g_in_isr = g_in_isr + 1;
    kickos_isr_irq(SCI6_RXI_VECTOR);
    g_in_isr = g_in_isr - 1;
}

// --- One-time core bring-up, called by the chip's arch_init -----------------
// Starts CMTW1 free-running (the monotonic clock) and resets the software state.
// The chip has already released the CMTW module stop and set the CMWCR prescale.
void kickos_rxv3_init(void)
{
    g_in_isr = 0;
    g_cyc_high = 0;
    g_cyc_last = 0;

    // SWINT is the deferred-switch line: lowest active priority, so a switch requested
    // from an ISR is accepted only after every other ISR drains, and below IPL_LOCK so an
    // IrqLock masks it and the switch fires as the lock releases. arch_switch pends it via
    // SWINTR; startup.S routes INTB[27] to kickos_rx_pendsw. SWINT + SWINT2 share this
    // IPR (regs.h).
    reg8(ICU_IPR_SWINT) = static_cast<uint8_t>(IPL_PENDSW);
    reg8(ICU_IER_BASE + (SWINT_VECTOR >> 3)) =
        static_cast<uint8_t>(reg8(ICU_IER_BASE + (SWINT_VECTOR >> 3)) |
                             (1u << (SWINT_VECTOR & 7)));

    // SWINT2 is the software-inject doorbell (arch_irq_inject -> kickos_rx_swint2). It
    // shares SWINT's IPR (IPL_PENDSW, set above) and needs its own IER bit enabled so
    // an injected logical line is delivered. startup.S routes INTB[26] to the ISR.
    icu_ier_set(SWINT2_VECTOR, true);

    // CMTW1: free-running 32-bit counter (CCLR=001 disables clearing, so it wraps
    // at 2^32 on its own), PCLK/8, no interrupt; read-extended in software (sec.5).
    // It is also the raw telemetry trace clock (arch_trace_now).
    reg16(CMTW1_BASE + CMTW_CMWSTR) = 0;
    reg32(CMTW1_BASE + CMTW_CMWCNT) = 0;
    reg32(CMTW1_BASE + CMTW_CMWCOR) = 0xFFFFFFFFu;
    reg16(CMTW1_BASE + CMTW_CMWCR) = CMWCR_CKS_PCLK8 | CMWCR_CCLR_FREERUN;
    reg16(CMTW1_BASE + CMTW_CMWSTR) = CMWSTR_STR;
}

}
