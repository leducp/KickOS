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

#include <stddef.h> // offsetof

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

    // User-RAM bump allocator (chip linker script defines the bounds).
    volatile uint32_t g_ram_used = 0;

    // Software in-ISR nesting counter (RX has no IPSR-equivalent). Bumped only by
    // the first-level DEVICE-IRQ dispatchers, never by the syscall INT path, so
    // arch_in_isr() reads false throughout syscall_dispatch (arch.h contract).
    volatile int g_in_isr = 0;
}

extern "C"
{
    // Userspace thread epilogue: a user thread whose entry returns must trap out
    // via the exit syscall (it cannot run the kernel epilogue with PM=1). Seeded
    // by arch_context_init as the fabricated frame's return address for a user
    // thread (kickos_thread_return for a kernel thread).
    void kickos_user_thread_return(void);

    // Linker-provided user-RAM bounds (chip linker script).
    extern unsigned char __kickos_ram_start[];
    extern unsigned char __kickos_ram_end[];

    // The CMTW input-clock frequency in Hz (PCLKB / prescale), defined by the
    // chip. Drives the ns<->cycle conversions for the clock + one-shot timer.
    extern uint32_t kickos_rx_timer_hz;

    // Shared with switch.S. volatile: written by C and by asm across a seam the
    // compiler cannot see.
    struct arch_context* volatile g_arch_current = nullptr;
    struct arch_context* volatile g_arch_next = nullptr;
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
        if (on)
        {
            ier = static_cast<uint8_t>(ier | bit);
        }
        else
        {
            ier = static_cast<uint8_t>(ier & ~bit);
        }
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

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
void arch_trace_stamp_id(struct arch_context* ctx, uint16_t id)
{
    ctx->trace_tid = id;
}
#endif

void arch_timer_arm(uint64_t deadline_ns)
{
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
    reg16(CMTW0_BASE + CMTW_CMWSTR) = 0;
    reg8(ICU_IR_BASE + CMWI0_VECTOR) = 0; // drop a pending compare-match request
}

// --- MPU: hardware enforcement is M2; no-op on M1 (matches armv7m) ----------
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n)
{
    (void)regions;
    (void)n;
    // M2: reprogram RSPAGEn/REPAGEn.UAC/V on switch-in (spike sec.4). No enforcement
    // on M1.x -- privilege + syscall only.
}

uintptr_t arch_ram_base(void)
{
    return reinterpret_cast<uintptr_t>(__kickos_ram_start);
}

size_t arch_ram_size(void)
{
    return static_cast<size_t>(__kickos_ram_end - __kickos_ram_start);
}

void* arch_ram_alloc(size_t size)
{
    size_t total = arch_ram_size();
    size_t need = (size + 31u) & ~static_cast<size_t>(31u); // 32-byte aligned
    arch_irq_state_t s = arch_irq_save();
    void* p = nullptr;
    if (need != 0 and need <= total - g_ram_used)
    {
        p = __kickos_ram_start + g_ram_used;
        g_ram_used += need;
    }
    arch_irq_restore(s);
    return p;
}

uintptr_t arch_mpu_probe_addr(void)
{
    return 0; // no enforced MPU on M1.x -> no probe address (real on M2)
}

// --- Interrupt controller (ICUD) --------------------------------------------
void arch_irq_mask(int line)
{
    if (line < 0)
    {
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
    // Program the source priority BELOW the kernel lock level before enabling, so
    // a device line cannot preempt an IrqLock-held section (the armv7m NVIC_IPR
    // care). TODO(HW): the ICUD shares IPR entries per a source table (IPR index
    // != vector in general, UM sec.15.2.4); this index==line write is correct only
    // for sources with a 1:1 IPR and must be replaced by the table on real HW.
    reg8(ICU_IPR_BASE + static_cast<unsigned>(line)) = static_cast<uint8_t>(IPL_DEVICE);
    icu_ier_set(line, true);
}

void arch_irq_inject(int irq)
{
    if (irq < 0)
    {
        return;
    }
    unsigned l = static_cast<unsigned>(irq);
    // Match the proven sim semantics: a masked line drops the raise.
    if ((reg8(ICU_IER_BASE + (l >> 3)) & (1u << (l & 7))) == 0)
    {
        return;
    }
    // RX cannot pend an arbitrary peripheral line from software (edge sources
    // accept only a 0 write to IRn.IR); only the two software interrupts are
    // software-settable (UM sec.15.2.5). SWINT is the context-switch line, so test
    // scaffolding routes injection to SWINT2; other lines drop. Real drivers never
    // inject.
    if (irq == SWINT2_VECTOR)
    {
        reg8(ICU_SWINT2R) = SWINT2R_SWINT2;
    }
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
    reg8(ICU_IR_BASE + CMWI0_VECTOR) = 0; // clear the request flag
    reg16(CMTW0_BASE + CMTW_CMWSTR) = 0;  // one-shot: stop until re-armed
    g_in_isr++;
    kickos_isr_timer(); // re-arms the next deadline
    g_in_isr--;
}

// Device-line default entry. TODO(HW): RX has no cheap current-vector read, so
// per-line dispatch (identifying `line` for kickos_isr_irq) needs either
// per-vector trampolines or a driver-supplied handler -- a driver-era concern;
// no device drivers exist at M1.x, so this is a safe stub.
__attribute__((interrupt)) void kickos_rx_default_irq(void)
{
    g_in_isr++;
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
