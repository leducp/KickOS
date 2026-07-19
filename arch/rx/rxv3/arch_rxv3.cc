// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RXv3 arch backend (Renesas RX72M): the parts of the arch.h seam that are RXv3
// core-generic (chip-independent). The context switch + syscall trap assembly
// lives in switch.S; the chip layer (arch/rx/chip/*) supplies the hardware edges
// -- arch_init (clocks + console + module-stop release + INTB), arch_console_write,
// arch_shutdown -- and the linker script that defines the user-RAM region and the
// CMTW input-clock frequency.
//
// BUILD-ONLY: there is no RX execution target (no QEMU model) in this
// environment. This compiles clean for RXv3 and the switch/syscall paths are
// validated by construction against the RXv3 ISA UM. NOT hardware-validated.

#include <kickos/arch/arch.h>
#include <kickos/units.h> // _s literal (== 1e9 ns) for the cycle<->ns conversions

#include "regs.h"
#include <kickos/console_tx.h> // console_tx_isr: drained by the TXI ISR below

#include <stddef.h> // offsetof

// Fault reporting (see the .fvectors shims in startup.S): the reporter calls
// kpanic_enter first, which masks IRQs, forces the synchronous polled writer, and
// flushes the ring -- so the dump is safe from an exception even though RX72M arms
// the buffered SCI6 console. kfault_terminate is the shared panic/fault dead-end
// (kernel.h).
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

// The SWINT switcher (switch.S) saves the full context: R1-R15 (single-precision
// FP lives in the GPR file), FPSW, the two accumulators, AND -- with -mdfpu (the
// rx72m board enables it for 64-bit doubles) -- the DPFPU register file (DR0-DR15
// + DPSW/DCMR/DECNT, via DPUSHM/DPOPM). arch_context_init fabricates the matching
// DPFPU slots below. If a future config drops -mdfpu, the DPUSHM/DPOPM ops and the
// fabricated slots simply carry zeroes for state the compiler never touches.
#if defined(__RX_DFPU_INSNS__)
static_assert(sizeof(double) == 8, "-mdfpu should give 64-bit doubles");
#endif

// switch.S hard-codes ctx.sp @0 (the saved SP). A silent reorder would corrupt
// the switch.
static_assert(offsetof(struct arch_context, sp) == 0, "switch.S expects ctx.sp @0");
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
// The SWINT switcher reads the trace id at 4[ctx] (the `4[r15]` loads in switch.S).
static_assert(offsetof(struct arch_context, trace_tid) == 4, "switch.S expects trace_tid @4");
#endif

// arch_irq_save raises PSW.IPL to the lock level with an MVTIPL immediate; keep
// the literal in the asm string in sync with the constant.
static_assert(kickos::rxv3::IPL_LOCK == 12, "arch_irq_save MVTIPL literal is #12");

namespace
{
    using namespace kickos::rxv3;

    // Free-running CMTW1 is 32-bit; extend to a monotonic 64-bit count in
    // software by catching wraps on each read (the armv7m DWT pattern). LIMITATION
    // (M1): a wrap not observed within one 2^32-cycle period is missed; a
    // counter-overflow interrupt is the refinement.
    volatile uint32_t g_cyc_high = 0;
    volatile uint32_t g_cyc_last = 0;

    // Software in-ISR nesting counter (RX has no IPSR-equivalent). Bumped only by
    // the first-level DEVICE-IRQ dispatchers, never by the syscall INT path, so
    // arch_in_isr() reads false throughout syscall_dispatch (arch.h contract).
    volatile int g_in_isr = 0;

    // Software IRQ controller for INJECTED logical lines. The RX ICU cannot pend an
    // arbitrary peripheral line from software (only the two software interrupts are
    // settable), so injected lines are delivered over the single SWINT2 doorbell:
    // g_inject_line carries the logical line, g_irq_masked gates it. Mirrors the
    // sim/xtensa/riscv model. Lines < kSoftIrqLines are software (this controller);
    // lines >= it are real ICU vectors gated by ICU.IER (e.g. console TXI6 = 87).
    // RX's own sub-32 vectors (SWINT 26/27, timer CMWI0 30) are configured directly
    // by the arch/chip init and never pass through the arch_irq_* seam, so they do
    // not collide. Masked-by-default (RX is a masked controller): a line is armed
    // only by arch_irq_unmask (kernel irq_register/irq_ack), like the ARM NVIC.
    constexpr int kSoftIrqLines = 32;
    volatile uint32_t g_irq_masked = 0xFFFFFFFFu;
    volatile int g_inject_line = -1;

    // Build-only MPU wedge localizer (DEFAULT OFF: -DKICKOS_RX_MPU_TRACE=1). Raw
    // polled SCI6 byte, bounded spin, touching no ring or global -- safe from ISR and
    // fault context. It interleaves with the buffered TAP output; the LAST byte before
    // the console goes silent localizes the wedge that appears at rr_interleave (the
    // first switch driven from the CMTW0 timer ISR under enforcement):
    //   'T' then silence          -> hang in ktime_on_timer/tick_rr BEFORE apply
    //   'T' '[' then silence      -> hang INSIDE arch_mpu_apply's MPU register writes
    //   'T' '[' ']' then silence  -> hang AFTER apply (pendsw switch / re-arm / switched-in)
    //   'T[]' flooding forever    -> timer/switch livelock (no forward progress)
    //   'F' anywhere              -> an access exception fired (a FAULT, not a hang)
    // The '[' / ']' pair is emitted only from ISR context (g_in_isr>0), so the thread-
    // context switches of tests 1-3 stay quiet and only the test-4 timer path prints.
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

    // Shared with switch.S. volatile: written by C and by asm across a seam the
    // compiler cannot see.
    struct arch_context* volatile g_arch_current = nullptr;
    struct arch_context* volatile g_arch_next = nullptr;

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
            g_cyc_high++;
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
        // ISR's arch_irq_mask on a sibling line -- callable from unlocked syscall
        // context (irq_attach/irq_unmask). Matches the riscv/xtensa mask primitives.
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

    // RX72M ICU IPR index for a vector. IR (UM sec.15.2.1) and IER (sec.15.2.2)
    // are indexed 1:1 by vector; only IPR (sec.15.2.4) is shared -- several sources
    // collapse onto one IPR entry, so the IPR index is NOT the vector number in
    // general. This table carries the mappings confirmed against the UM interrupt
    // vector table (sec.15.3.1); an unlisted vector falls back to identity, which
    // holds for the 1:1 SCIg/peripheral block this backend arms today (SCI6 TXI6 =
    // vector 87 -> IPR087). A shared source NOT listed here would be given the
    // wrong IPR: add it (and cite the UM) before enabling that device line.
    struct ipr_map_entry
    {
        uint16_t vector;
        uint8_t ipr;
    };
    constexpr ipr_map_entry kIprMap[] = {
        {SWINT2_VECTOR, 3}, // SWINT2(26)+SWINT(27) share ICU.IPR[3] (RX72x BSP)
        {SWINT_VECTOR, 3},
        {CMWI0_VECTOR, 6},  // CMTW0 CMWI0(30) -> ICU.IPR[6]
    };

    inline unsigned vector_to_ipr(int vector)
    {
        for (unsigned i = 0; i < sizeof(kIprMap) / sizeof(kIprMap[0]); i++)
        {
            if (kIprMap[i].vector == vector)
            {
                return kIprMap[i].ipr;
            }
        }
        return static_cast<unsigned>(vector); // identity: 1:1 IPR sources
    }
}

// ===========================================================================
extern "C"
{

// --- Context init: fabricate a full switch-in frame (see switch.S layout) ---
// The frame is identical to what the SWINT switcher saves, so the first switch-in
// (arch_start) restores it and RTEs into entry(arg). Low->high on the USP:
//   [+0 A0LO][+4 A0HI][+8 A0GU][+12 A1LO][+16 A1HI][+20 A1GU][+24 FPSW]
//   [+28 R1=arg .. +84 R15][+88 PC=entry][+92 PSW]
// and, ABOVE the frame, [+96] = the address a returning entry() RTSes into
// (kickos_thread_return for a kernel thread, kickos_user_thread_return for a user
// one -- which traps out via the exit syscall since it cannot run the kernel
// epilogue with PM=1). RTE pops PC then PSW, delivering R1=arg per the psABI.
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
    // DPFPU register file, banked below the accumulators (switch.S DPUSHM/DPOPM).
    // A fresh thread's DR0-DR15 are don't-care (it sets them before first use), so
    // zero them; the control words take the DPFPU reset posture (DPSW round-to-
    // nearest, DECNT=1) per the RXv3 ISA UM DPFPU reset values. Layout must
    // mirror DPUSHM.D dr0-dr15 (higher) + DPUSHM.L dpsw-decnt (lower): so DR block
    // first (all zero), then DECNT, DCMR, DPSW with DPSW lowest = the new ctx.sp.
    for (int i = 0; i < 16 * 2; i++)             // DR0-DR15 (16 doubles, 2 words each)
    {
        *(--sp) = 0;
    }
    *(--sp) = 1;                                 // DECNT (reset value 1)
    *(--sp) = 0;                                 // DCMR
    *(--sp) = 0x00000100u;                       // DPSW (lowest word)
#endif

    ctx->sp = reinterpret_cast<uint32_t>(sp);
}

// --- Critical section: raise PSW.IPL to the kernel lock level ---------------
arch_irq_state_t arch_irq_save(void)
{
    uint32_t psw;
    __asm volatile("mvfc psw, %0" : "=r"(psw));
    // Raise to IPL_LOCK (12). MVTIPL takes an immediate only, so the level is a
    // literal (static_assert above pins it). MVTIPL is self-synchronizing -- no
    // DSB/ISB dance is needed (that concern is ARM's BASEPRI-specific).
    __asm volatile("mvtipl #12" ::: "memory");
    return (psw & PSW_IPL_MASK) >> PSW_IPL_SHIFT; // old IPL
}

void arch_irq_restore(arch_irq_state_t state)
{
    // Restore only the IPL field (MVTIPL can't take a runtime value; write the
    // whole PSW via MVTC with just the IPL bits replaced -- PM is ignored on write
    // in supervisor, flags/U/I are preserved from the current PSW).
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

// C side of the RX exception handler (startup.S .fvectors shims branch here with
// r1=cause [the fixed-vector offset], r2=stacked PC, r3=stacked PSW). Dump the
// context, then hand off to the shared dead-end (blink on real HW). Runs on the
// ISP in supervisor mode. kpanic_enter masks IRQs (raises PSW.IPL, never restored --
// this path does not return), forces the polled writer, and flushes the ring.
void kickos_rx_fault_report(uint32_t cause, uint32_t pc, uint32_t psw)
{
    rx_mpu_mark('F'); // localizer: an exception fired (a FAULT, not a hang) -- raw, pre-console
    kpanic_enter();
#if KICKOS_HAVE_MPU
    // The access exception (fixed vector +0x54) IS the RX MPU violation, and the RX
    // MPU checks user mode only (UM sec.17.1.1) -- so one taken with the faulting
    // PSW.PM set is an unprivileged thread hitting an ungranted region. Route it to
    // the shared reporter that names the task + address, exactly like the riscv PMP
    // and ARM MemManage paths. MPESTS.DMPER => operand access (address in MPDEA, DRW
    // gives read vs write); MPESTS.IMPER => instruction fetch (address is the stacked
    // PC). An access exception from supervisor cannot be an MPU fault (supervisor is
    // never checked), so it falls through to the generic dump = a genuine kernel bug.
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
// The free-running 32-bit CMTW1 counter IS a natural raw trace clock: u32, wraps
// on its own, host reconstructs absolute time from the SESSION clock_hz anchors
// (arch.h). No extra hardware spent -- same source as arch_clock_now, read raw
// (no ns conversion, no wrap-extend). KICKOS_HAVE_TRACE_CLOCK is set for rxv3.
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
    // Idempotent re-arm: ktime_rearm calls this on EVERY context switch. If the
    // one-shot is already running toward this exact deadline, leave CMWCNT alone --
    // resetting it to 0 each switch (players ping-ponging faster than the deadline)
    // means the compare is never reached and a far deadline (e.g. a reporter's 0.5s
    // sleep) starves. "Running toward it" is tracked purely in software: the timer
    // ISR sets g_rx_armed_ns = ~0 before it re-arms, so its own re-arm is never
    // skipped, and a reschedule with the same pending deadline is. (An earlier guard
    // read CMWSTR.STR to decide this, but that HW readback raced at full switch speed
    // -- the guard intermittently failed, reset CMWCNT, and the far deadline starved
    // on silicon whenever the CPU never idled.)
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
    // One-shot from zero: stop, load compare, clear counter, enable interrupt +
    // clear-on-match, start. (CMWCR clock/prescale is set once by the chip init.)
    reg16(CMTW0_BASE + CMTW_CMWSTR) = 0;
    reg32(CMTW0_BASE + CMTW_CMWCNT) = 0;
    reg32(CMTW0_BASE + CMTW_CMWCOR) = static_cast<uint32_t>(cyc);
    // 32-bit up-counter, PCLK/8, clear-on-CMWCOR-match, compare-match interrupt.
    reg16(CMTW0_BASE + CMTW_CMWCR) = CMWCR_CKS_PCLK8 | CMWCR_CCLR_ON_MATCH | CMWCR_CMWIE;
    // Gate that actually arms the CMWCOR compare (else the counter free-runs past
    // it -- no clear, no CMWI). Reset value is 0. Idempotent; rewritten each arm so
    // no path can leave it clear. (First-silicon fix; UM sec.32.2.3.)
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
// On RX the MPU checks accesses ONLY in user mode; supervisor is never checked
// and always permitted (UM sec.17.1.1 / the sec.17.3.4 flow). So a PRIVILEGED
// (PM=0) thread keeps full access no matter what these registers hold, and there
// is no K64F-style supervisor-field hazard to guard against -- there is simply no
// supervisor field. Enforcement therefore reduces to: a no-access background
// (MPBAC=0) so a user thread faults everywhere it has no explicit region, plus
// the running thread's regions loaded into the eight RSPAGEn/REPAGEn slots.
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n)
{
#if KICKOS_HAVE_MPU
    if (g_in_isr)
    {
        rx_mpu_mark('['); // localizer: entering the MPU register writes from ISR ctx
    }
    // One-time: background = no user access (UBAC=0), then enable. Overlaps OR
    // their permission bits with the background (UM sec.17.1.4), so a nonzero
    // MPBAC would silently grant every user thread -- it must stay 0. MPOPI.INV
    // clears any stale region V bits (reset leaves REPAGEn.V=0, but be explicit).
    // MPU registers are supervisor-only and not PRCR-gated (UM Table 13.1), so no
    // unlock. MPEN takes effect on the RTE into user mode (UM sec.17.2.3).
    static bool mpu_ready = false;
    if (not mpu_ready)
    {
        reg16(MPU_MPOPI) = MPU_MPOPI_INV;
        reg32(MPU_MPBAC) = 0;
        reg32(MPU_MPEN) = MPU_MPEN_MPEN;
        mpu_ready = true;
    }
    // Skip the register rewrite when the incoming set already matches what the MPU
    // holds. RR ping-pong between two PRIVILEGED threads reprograms the IDENTICAL
    // kernel-domain region on every timer tick, and RX supervisor is never checked,
    // so the registers already describe a correct set -- rewriting them (from the
    // timer ISR, at RR frequency) only adds MPU-bus traffic. Compare against the
    // last-applied set; on a match the hardware is already correct, so return.
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
        // Load regions[0..n) into slots 0..n-1; invalidate the rest. Write RSPAGEn
        // (start) BEFORE REPAGEn, and put V in the REPAGEn write so a slot is never
        // momentarily valid with a stale end/attr.
        // A region the 16-byte pages cannot represent EXACTLY is fail-closed (slot left
        // V=0), never masked wider -- rounding base/end to page bounds would grant up to
        // 15 bytes beyond the region on each side (matches ARM PMSA / RISC-V PMP skip).
        for (size_t i = 0; i < MPU_REGION_COUNT; i++)
        {
            uintptr_t const rsp = MPU_RSPAGE_BASE + i * MPU_REGION_STRIDE;
            uintptr_t const rep = MPU_REPAGE_BASE + i * MPU_REGION_STRIDE;
            if (i < n and arch_mpu_region_encodable(regions[i].base, regions[i].size))
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
                // The low-nibble masks are RSPAGE/REPAGE field encoding (page-exact by
                // the encodable gate above; REPAGE[3:0] carry UAC/V), not rounding.
                reg32(rsp) = static_cast<uint32_t>(base) & MPU_PAGE_MASK;
                reg32(rep) = (static_cast<uint32_t>(end) & MPU_PAGE_MASK) | uac | MPU_REPAGE_V;
            }
            else
            {
                reg32(rep) = 0; // clears V -> slot inactive
            }
        }
        // UM sec.17.4.3: read back an MPU register so the writes are in effect before
        // the scheduler's RTE drops into user mode -- the RX visibility barrier (the
        // ARM DSB/ISB analog). The asm consumes the value so the volatile load is
        // really issued and is not reordered past here.
        uint32_t const mpu_sync = reg32(MPU_MPEN);
        __asm volatile("" ::"r"(mpu_sync) : "memory");
        // Cache the applied set so an identical follow-up switch skips the rewrite.
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
#else
    (void)regions;
    (void)n;
#endif
}

size_t arch_mpu_min_region(void)
{
    // RX MPU page = 16 bytes (RSPAGEn/REPAGEn address[31:4], UM sec.17.1.2). Like
    // SYSMPU the hardware takes arbitrary page-granular bounds, so the pow2 shaping
    // the shared alloc/linker path applies is a describable superset, not a
    // requirement -- returning the page size keeps every descriptor representable.
    return 16u;
}

// The RX MPU is byte-granular on a 16-byte page (RSPAGEn/REPAGEn hold addr[31:4]);
// a window is exact iff base and base+size both land on a 16-byte boundary. No
// power-of-two size is required.
bool arch_mpu_region_encodable(uintptr_t base, size_t size)
{
    if (size < 16u)
    {
        return false;
    }
    return (base & 15u) == 0 and (size & 15u) == 0;
}



// --- Interrupt controller (ICUD) --------------------------------------------
void arch_irq_mask(int line)
{
    if (line < 0)
    {
        return;
    }
    if (line < kSoftIrqLines)
    {
        g_irq_masked |= (1u << static_cast<unsigned>(line));
        return;
    }
    icu_ier_set(line, false);
}

void arch_irq_unmask(int line)
{
    if (line < 0)
    {
        return;
    }
    if (line < kSoftIrqLines)
    {
        g_irq_masked &= ~(1u << static_cast<unsigned>(line));
        return;
    }
    // Program the source priority BELOW the kernel lock level before enabling, so
    // a device line cannot preempt an IrqLock-held section (the armv7m NVIC_IPR
    // care). IPR is shared per the ICU source table, so the index comes from
    // vector_to_ipr -- NOT the vector (IR/IER stay vector-indexed via icu_ier_set).
    reg8(ICU_IPR_BASE + vector_to_ipr(line)) = static_cast<uint8_t>(IPL_DEVICE);
    icu_ier_set(line, true);
}

void arch_irq_inject(int irq)
{
    // Only logical lines are injectable. A real peripheral line cannot be pended from
    // software on RX, and drivers never inject -- so anything >= kSoftIrqLines drops.
    if (irq < 0 or irq >= kSoftIrqLines)
    {
        return;
    }
    // A masked line drops the raise (the proven sim semantics).
    if ((g_irq_masked & (1u << static_cast<unsigned>(irq))) != 0)
    {
        return;
    }
    g_inject_line = irq;                // recorded BEFORE the doorbell (the ISR reads it)
    reg8(ICU_SWINT2R) = SWINT2R_SWINT2; // ring SWINT2 -> kickos_rx_swint2 dispatches it
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
    g_in_isr++;
    kickos_isr_timer(); // re-arms the next deadline
    g_in_isr--;
}

// Buffered-console drain ISR. Routed from INTB[87] (SCI6 TXI6) by the chip's
// startup.S; the ring/backend/line come from the chip via arch_console_tx_backend,
// and console_buffer_init unmasks the line + arms the ring. TXI6 is edge-triggered
// (UM sec.15.3.1): the ICU clears IR087 on accept, so no source flag is touched
// here -- console_tx_isr pushes bytes and gates SCR.TIE, which re-arms the edge.
__attribute__((interrupt)) void kickos_rx_console_txi_isr(void)
{
    g_in_isr++;
    console_tx_isr();
    g_in_isr--;
}

// SWINT2 doorbell: the software IRQ controller's delivery vector. arch_irq_inject
// latched the logical line in g_inject_line and pended SWINT2; run its bound handler.
// Clear IR026 first -- a software interrupt's request flag is NOT auto-cleared on
// accept (the same omission that livelocked SWINT), so leaving it set re-fires the
// doorbell forever. kickos_isr_irq runs the handler, which masks the line itself
// (irq_event_isr / the null-object default), so no masking here.
__attribute__((interrupt)) void kickos_rx_swint2(void)
{
    reg8(ICU_IR_BASE + SWINT2_VECTOR) = 0;
    g_in_isr++;
    int line = g_inject_line;
    g_inject_line = -1;
    if (line >= 0)
    {
        kickos_isr_irq(line);
    }
    g_in_isr--;
}

// Chip hook: name the pending real-device line for the shared default ISR. The
// INTB routes EVERY device source to kickos_rx_default_irq and the RXv3 core has
// no cheap current-vector read, so the first-level ISR cannot identify the line on
// its own -- a chip that wires a real peripheral overrides this to read its
// source's IR/status flag and return the vector (>= 0), or -1 for "none pending".
// Weak no-op default returns -1: the M1.x posture wires no real device (only
// injected logical lines, delivered over SWINT2). Mirrors the riscv
// kickos_rv_ext_dispatch_dev weak chip hook.
__attribute__((weak)) int kickos_rx_dev_pending_line(void) { return -1; }

// Device-line default entry: the real first-level ISR for every INTB device slot.
// The chip hook names the fired line (the one fact RX cannot derive); the rest is
// the generic first-level sequence, identical to the SWINT2 inject path above and
// the riscv .Lextdev dispatch -- clear the ICU edge-request flag, then post the
// bound target. kickos_isr_irq runs the handler, which masks the line at the ICU
// IER (irq_event_isr / the null-object default), so the unprivileged driver
// services it without a re-fire; no masking here. (A level source keeps IRn
// asserted -- writing 0 is then a no-op -- but the IER mask still gates re-entry.)
// BUILD-ONLY: with no chip override the hook returns -1 and this is inert, exactly
// the prior stub; a real routed peripheral needs RX silicon to validate.
__attribute__((interrupt)) void kickos_rx_default_irq(void)
{
    g_in_isr++;
    int line = kickos_rx_dev_pending_line();
    // Bound the hook's line to the RX ICU IR register space (256 vectors, one byte each)
    // before indexing it: a bogus vector from a future chip override must not scribble an
    // arbitrary register (the reg8 write precedes kickos_isr_irq's own range check).
    if (line >= 0 and line < 256)
    {
        reg8(ICU_IR_BASE + static_cast<unsigned>(line)) = 0;
        kickos_isr_irq(line);
    }
    g_in_isr--;
}

// --- One-time core bring-up, called by the chip's arch_init -----------------
// Starts CMTW1 free-running (the monotonic clock) and resets the software state.
// The chip has already released the CMTW module stop and set the CMWCR prescale.
void kickos_rxv3_init(void)
{
    g_in_isr = 0;
    g_cyc_high = 0;
    g_cyc_last = 0;

    // SWINT is the deferred-switch line (the PendSV analog): lowest active priority
    // so a switch requested from an ISR is accepted only after every other ISR
    // drains, and below IPL_LOCK so an IrqLock masks it (the switch fires as the
    // lock releases). arch_switch pends it via SWINTR; startup.S routes INTB[27] to
    // kickos_rx_pendsw. SWINT + SWINT2 share this IPR (regs.h).
    reg8(ICU_IPR_SWINT) = static_cast<uint8_t>(IPL_PENDSW);
    reg8(ICU_IER_BASE + (SWINT_VECTOR >> 3)) =
        static_cast<uint8_t>(reg8(ICU_IER_BASE + (SWINT_VECTOR >> 3)) |
                             (1u << (SWINT_VECTOR & 7)));

    // SWINT2 is the software-inject doorbell (arch_irq_inject -> kickos_rx_swint2). It
    // shares SWINT's IPR (IPL_PENDSW, set above) and needs its own IER bit enabled so
    // an injected logical line is delivered. startup.S routes INTB[26] to the ISR.
    icu_ier_set(SWINT2_VECTOR, true);

    // CMTW1: free-running 32-bit counter (CCLR=001 disables clearing, so it wraps
    // at 2^32 on its own), PCLK/8, no interrupt -- read-extended in software (sec.5).
    // It is also the raw telemetry trace clock (arch_trace_now).
    reg16(CMTW1_BASE + CMTW_CMWSTR) = 0;
    reg32(CMTW1_BASE + CMTW_CMWCNT) = 0;
    reg32(CMTW1_BASE + CMTW_CMWCOR) = 0xFFFFFFFFu;
    reg16(CMTW1_BASE + CMTW_CMWCR) = CMWCR_CKS_PCLK8 | CMWCR_CCLR_FREERUN;
    reg16(CMTW1_BASE + CMTW_CMWSTR) = CMWSTR_STR;
}

}
