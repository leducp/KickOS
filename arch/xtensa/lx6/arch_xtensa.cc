// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
#include <kickos/arch/arch.h>
#include <kickos/arch/xtensa_frame.h> // F_* interrupt-frame offsets, shared with startup.S
#include <kickos/units.h> // _s literal (== 1e9 ns) for the cycle<->ns conversions
#include <kickos/trace/record.h> // ArchId: pin this build's trace-arch id to this backend
#include <kickos/sys/atomic.h>

#include <stddef.h> // offsetof

// The trace-arch id (CMake ladder / this chip's caps.cmake) must equal the ArchId
// for the arch this backend implements, or a SESSION record mislabels the trace.
// A wrong caps.cmake value breaks the build here instead of drifting silently.
static_assert(KICKOS_TRACE_ARCH == kickos::trace::ARCH_XTENSA,
              "KICKOS_TRACE_ARCH does not match ArchId::ARCH_XTENSA for lx6");

// Fault reporting (see the _kickos_lx6_fault shim in startup.S, which captures the
// fault special registers and calls here with the window ABI live): the reporter
// calls kpanic_enter first, which masks IRQs, forces the synchronous polled writer,
// and flushes the ring. This is load-bearing on ESP32: the shim runs at INTLEVEL=15
// with g_isr_depth still 0, so without forcing the sync path the dump would enqueue
// into the buffered UART0 ring whose drain interrupt is masked, and be lost.
// kfault_terminate is the shared panic/fault dead-end (kernel.h).
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

// switch.S + startup.S hard-code these arch_context field offsets; keep struct and
// asm in sync (a silent reorder would corrupt the saved SP / PS / return-PC / the
// resume-path discriminator on switch).
static_assert(offsetof(struct arch_context, sp) == 0, "asm expects ctx.sp @0");
static_assert(offsetof(struct arch_context, ps) == 4, "asm expects ctx.ps @4");
static_assert(offsetof(struct arch_context, pc) == 8, "asm expects ctx.pc @8");
static_assert(offsetof(struct arch_context, resume_kind) == 12,
              "asm (CTX_KIND) expects ctx.resume_kind @12");
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
static_assert(offsetof(struct arch_context, trace_tid) == 16,
              "asm (CTX_TID) telemetry hook expects ctx.trace_tid @16");
#endif

namespace
{
    using kickos::Atomic;
    using kickos::Order;

    // PS fields (Xtensa ISA / corebits.h): EXCM=bit4, UM=bit5, CALLINC=bits[17:16],
    // WOE=bit18. A resumed thread runs windowed (WOE=1) with interrupts enabled
    // (INTLEVEL=0).
    constexpr uint32_t PS_UM = 0x20;
    constexpr uint32_t PS_EXCM = 0x10;
    constexpr uint32_t PS_WOE = 0x40000;
    constexpr uint32_t PS_CALLINC1 = 0x1u << 16; // CALLINC=1 (the trampoline `entry` rotates by this)

    // Interrupt-frame field accessor. The frame offsets (F_PC/F_PS/F_A0/F_SIZE, the
    // F_AREG stride, ...) are single-sourced in kickos/arch/xtensa_frame.h and shared
    // verbatim with the save/restore asm in startup.S.
    inline uint32_t f_areg(unsigned n) { return F_AREG(n); }

    // Critical-section interrupt level: mask the C-handleable levels 1-3 (the timer
    // + all device lines), leaving the high-level 4-7 / NMI zero-latency band
    // unmaskable.
    constexpr uint32_t KICKOS_IRQ_LOCK_LEVEL = 3;

    // ESP32 CCOMPARE0 (Xtensa timer 0) is wired to per-CPU internal interrupt 6, a
    // level-1 (C-handleable) line (ESP32 TRM 4.3, "CPU interrupts" table).
    constexpr int CCOMPARE0_INT = 6;

    // ESP32 internal interrupt 7 is the level-1 SOFTWARE interrupt: the only L1 line
    // arch_irq_inject can latch (via INTSET). Unlike a device/level line it is not
    // cleared at a peripheral source, so the dispatcher must wsr.intclear it or one
    // inject redelivers forever.
    constexpr int SW_INT_L1 = 7;

    // Level-1 interrupt line mask (XCHAL_INTLEVEL1_MASK for the LX6 config): only
    // these lines are C-handleable via the level-1 entry; higher-level lines have
    // their own vectors. The dispatcher restricts pending to this set.
    constexpr uint32_t KICKOS_L1_INT_MASK = 0x000637FFu;

    // Wrap extension of the 32-bit CCOUNT cycle counter. LIMITATION: at 240 MHz CCOUNT
    // wraps every ~17.9 s, and a wrap not observed within one 2^32-cycle period is missed.
    // The two words are ONE value, kept coherent by the IrqLock in now_cycles.
    uint32_t g_cyc_high = 0;
    uint32_t g_cyc_last = 0;

    inline uint32_t rd_ccount()
    {
        uint32_t v;
        __asm volatile("rsr.ccount %0" : "=a"(v));
        return v;
    }
    inline uint32_t rd_intenable()
    {
        uint32_t v;
        __asm volatile("rsr.intenable %0" : "=a"(v));
        return v;
    }
    inline void wr_intenable(uint32_t v)
    {
        __asm volatile("wsr.intenable %0" ::"a"(v) : "memory");
    }
    inline uint32_t rd_interrupt()
    {
        uint32_t v;
        __asm volatile("rsr.interrupt %0" : "=a"(v));
        return v;
    }

    // PHYSICAL interrupt enable/disable (INTENABLE), for the timer (CCOMPARE0) and the
    // int-7 software doorbell. NOT the public arch_irq_* seam, which is the software
    // controller over LOGICAL device lines.
    inline void phys_int_enable(uint32_t bit)
    {
        arch_irq_state_t s = arch_irq_save();
        wr_intenable(rd_intenable() | bit);
        arch_irq_restore(s);
    }
    inline void phys_int_disable(uint32_t bit)
    {
        arch_irq_state_t s = arch_irq_save();
        wr_intenable(rd_intenable() & ~bit);
        arch_irq_restore(s);
    }

    // Real device lines: the matrix routes a peripheral source to a CPU interrupt, and
    // the chip demuxes that CPU interrupt into logical lines. INTENABLE is their
    // kernel-owned mask (RULE L1) because the peripheral's own enable register sits
    // inside the block granted to the driver.
    // The mask is COARSE: several lines may name the same cpu_int, and masking one of
    // them masks its siblings.
    constexpr unsigned LX6_DEV_ROUTES = 4;
    Atomic<int8_t, Order::RELAXED> g_dev_cpu_int[LX6_DEV_ROUTES] = {-1, -1, -1, -1};
    Atomic<int8_t, Order::RELAXED> g_dev_line[LX6_DEV_ROUTES] = {-1, -1, -1, -1};

    // The CPU interrupt a logical line's mask targets, or -1 for a line with no route
    // (every injected software line, whose mask is the g_irq_masked bit alone).
    inline int dev_route_cpu_int(int line)
    {
        int8_t const want = static_cast<int8_t>(static_cast<unsigned>(line) & 31u);
        for (unsigned i = 0; i < LX6_DEV_ROUTES; i++)
        {
            if (g_dev_line[i] == want)
            {
                return g_dev_cpu_int[i];
            }
        }
        return -1;
    }
}

extern "C"
{
    // The core clock in Hz, defined + maintained by the chip backend.
    extern uint32_t SystemCoreClock;

    // The windowed cooperative swap + the fresh-thread trampoline (switch.S).
    void xtensa_switch(struct arch_context* from, struct arch_context* to);
    void _thread_trampoline(void);

    // Chip device dispatch: ISR context, once per asserted device CPU interrupt. The chip
    // reads its own peripheral status, calls kickos_isr_irq() once per asserted sub-source
    // (0..N logical lines), and owns the per-source clear discipline.
    // No <symbol>_default.cc fallback on purpose: an lx6 chip that binds a device route
    // and does not define this must fail to LINK, never link clean and drop every device
    // interrupt at runtime.
    void kickos_lx6_dispatch_dev(int cpu_int);

    // Shared with switch.S/arch_start/startup.S: the ctx of the running thread, and
    // the deferred-switch target when arch_switch is called from ISR context. Written
    // by C and by asm.
    kickos::Atomic<struct arch_context*, kickos::Order::RELAXED> g_arch_current = nullptr;
    kickos::Atomic<struct arch_context*, kickos::Order::RELAXED> g_arch_next = nullptr;

    // switch.S and the chip startup.S load each as a plain word at offset 0. Nothing else
    // enforces the layout.
    static_assert(sizeof(g_arch_current) == sizeof(struct arch_context*), "asm reads one word");
    static_assert(sizeof(g_arch_next) == sizeof(struct arch_context*), "asm reads one word");
    static_assert(alignof(decltype(g_arch_current)) == alignof(struct arch_context*), "asm reads it naturally aligned");

    // Set by arch_switch when it defers a switch from ISR context; consumed (and
    // cleared) by the level-1 interrupt exit (startup.S _kickos_int_level1), which
    // completes the swap on the way back to thread level. Distinct from comparing
    // g_arch_current vs g_arch_next: the cooperative path advances g_arch_current
    // without touching g_arch_next, so a stale g_arch_next must NOT be mistaken for
    // a pending preemption; this flag is the unambiguous request.
    kickos::Atomic<uint32_t, kickos::Order::RELAXED> g_arch_switch_pending = 0;

    // In-ISR depth (the IPSR!=0 analog). Maintained by the level-1 interrupt entry
    // (startup.S). arch_in_isr() reads it; the kernel uses it to forbid blocking
    // from ISR context and to defer a switch to interrupt exit.
    uint32_t g_isr_depth = 0;

    // startup.S carries both with l32i/s32i at offset 0. Nothing else enforces the width.
    static_assert(sizeof(g_arch_switch_pending) == 4, "asm reads one word");
    static_assert(sizeof(g_isr_depth) == 4, "asm reads one word");

    // Software interrupt controller for the LOGICAL device lines (arch.h inject/
    // mask contract), decoupled from the physical Xtensa interrupts. Xtensa INTSET
    // only latches the software-type lines (int 7/29), so a logical line cannot be a
    // physical bit; a raise records the line here and rings the ONE real software
    // doorbell (int 7), whose dispatcher then services this line. The timer (CCOMPARE0)
    // and the int-7 doorbell are the only PHYSICAL lines, driven via INTENABLE directly.
    // 1 = masked. All lines start MASKED at reset (the arch.h reset contract); a driver
    // unmasks its line (arch_irq_unmask, or irq_claim) before use.
    static uint32_t g_irq_masked = 0xFFFFFFFFu;
    // pending software-injected logical line
    static kickos::Atomic<int, kickos::Order::RELAXED> g_inject_line = -1;
    // bit set = a raise landed on this logical line while masked (latched one-deep,
    // coalesced). Redelivered through the int-7 doorbell at unmask.
    static uint32_t g_irq_pending = 0;

}

namespace
{
    using namespace kickos::units; // _s == 1e9 ns

    inline uint64_t now_cycles()
    {
        // The wrap-extend read must be atomic against a concurrent reader (thread
        // + ISR), so run it under the crit section.
        arch_irq_state_t s = arch_irq_save();
        uint32_t cur = rd_ccount();
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
        uint64_t f = SystemCoreClock;
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
        uint64_t f = SystemCoreClock;
        return (ns * f) / 1_s;
    }
}

// ===========================================================================
extern "C"
{

// --- Context init: fabricate a first-resume interrupt frame -------------------
// A fresh thread is started via the SAME rfe restore path as a preempted one
// (_kickos_lx6_irq_restore, startup.S), NOT a fabricated `retw` underflow: the thread
// enters the trampoline through rfe with PS.CALLINC=1 and the trampoline's own `entry`
// prologue establishes a proper CALL4 window frame. A retw start leaves the trampoline a
// phantom windowed frame with no valid base-save-area caller linkage; a non-blocking exit
// (entry->run->exit with no intervening block) descends deep enough to wrap WindowBase
// around the whole 64-AR file, forcing that phantom frame to overflow through invalid
// linkage, which corrupts the return PC and branches into data RAM.
//
// `entry` rotates the window by PS.CALLINC and, for a call4 frame, maps the caller's
// a6/a7 to the callee's a2/a3 (Xtensa ISA, Windowed Register Option), so the entry fn and
// arg go in the frame's a6/a7 and the outermost a0 is 0 (the trampoline never returns,
// and a 0 return terminates any spill/backtrace).
void arch_context_init(struct arch_context* ctx,
                       void (*entry)(void*), void* arg,
                       void* stack_base, size_t stack_size,
                       int privileged)
{
    (void)privileged; // no privilege split on this core

    uintptr_t top = reinterpret_cast<uintptr_t>(stack_base) + stack_size;
    top &= ~static_cast<uintptr_t>(15);        // 16-byte stack alignment (ABI)

    uint32_t frame_base = static_cast<uint32_t>(top) - F_SIZE;
    uint8_t* fb = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(frame_base));
    for (uint32_t i = 0; i < F_SIZE; i += 4)
    {
        *reinterpret_cast<uint32_t*>(fb + i) = 0;
    }
    auto set = [fb](uint32_t off, uint32_t v)
    { *reinterpret_cast<uint32_t*>(fb + off) = v; };

    set(F_PC, reinterpret_cast<uint32_t>(_thread_trampoline));
    // EXCM set so the restore's PS write is not interruptible mid-sequence; rfe
    // clears it. CALLINC=1 so the trampoline's `entry` opens a CALL4 frame and
    // rotates a6/a7 -> a2/a3. INTLEVEL=0: the thread runs with interrupts enabled.
    set(F_PS, PS_UM | PS_WOE | PS_EXCM | PS_CALLINC1);
    set(f_areg(1), static_cast<uint32_t>(top));         // a1 = stack top (entry's input SP)
    set(f_areg(6), reinterpret_cast<uint32_t>(entry));  // a6 -> trampoline a2 (entry fn)
    set(f_areg(7), reinterpret_cast<uint32_t>(arg));    // a7 -> trampoline a3 (arg)
    // a0/a4 already 0: a0 is discarded by the entry rotation; a4 becomes the
    // trampoline's a0 (its return address), where 0 is the safe outermost value.

    ctx->sp = frame_base;                               // base of the interrupt frame
    ctx->ps = 0;                                        // unused for an IRQ-resumed thread
    ctx->pc = 0;                                        // (PS/PC live in the frame)
    ctx->resume_kind = KICKOS_RESUME_IRQ;              // enters via rfe (irq_restore)
}

// The whole seam on this backend, and the resume_kind write above is what makes it whole:
// a thread that blocked cooperatively is saved as KICKOS_RESUME_COOP, so a rebuild that
// left resume_kind alone would send the switcher down the retw path onto a fabricated
// INTERRUPT frame. arch_context_init overwrites it, which is exactly what is wanted --
// forcing COOP here would be the bug, since a retw start leaves the trampoline a phantom
// windowed frame (see the note above arch_context_init).
//
// `privileged` is discarded on this core: PS.UM is 1 for kernel and thread alike, so
// there is no ring to restore and none to escalate through.
void arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                       void* stack_base, size_t stack_size)
{
    arch_context_init(ctx, entry, nullptr, stack_base, stack_size, 1);
}

// --- Switch: synchronous in thread context; deferred in ISR context ----------
void arch_switch(struct arch_context* from, struct arch_context* to)
{
    if (g_isr_depth != 0)
    {
        // Preemption: record the target and flag the request. The physical swap is
        // completed at level-1 interrupt exit (startup.S _kickos_int_level1), which
        // saves the interruptee (g_arch_current) in the interrupt-frame format and
        // resumes `to`.
        g_arch_next = to;
        g_arch_switch_pending.store(1);
        return;
    }
    g_arch_current = to;
    xtensa_switch(from, to);
}

// --- Critical section: RSIL to the kernel lock level (mask levels 1-3) --------
arch_irq_state_t arch_irq_save(void)
{
    uint32_t ps;
    // RSIL atomically returns the old PS and raises PS.INTLEVEL. Nesting-safe: the
    // whole PS is saved/restored, so a prior raised level is preserved.
    __asm volatile("rsil %0, %1" : "=a"(ps) : "i"(KICKOS_IRQ_LOCK_LEVEL) : "memory");
    return ps;
}

void arch_irq_restore(arch_irq_state_t state)
{
    __asm volatile("wsr.ps %0; rsync" ::"a"(static_cast<uint32_t>(state)) : "memory");
}

int arch_in_isr(void)
{
    return g_isr_depth != 0;
}

// --- Level-1 interrupt dispatch (called by startup.S _kickos_int_level1) ------
// Runs at INTLEVEL=1 in thread-style windowed context on the interruptee stack.
// g_isr_depth is bumped by the asm entry, so arch_in_isr() reads true here.
void kickos_lx6_dispatch_l1(void)
{
    uint32_t pending = rd_interrupt() & rd_intenable() & KICKOS_L1_INT_MASK;

    // Timer (CCOMPARE0), a PHYSICAL line: disabling it in INTENABLE stops re-fire
    // until the kernel re-arms (which clears the compare match; the pending bit is
    // level-triggered off the comparator).
    if ((pending & (1u << CCOMPARE0_INT)) != 0)
    {
        phys_int_disable(1u << CCOMPARE0_INT);
        kickos_isr_timer();
    }

    // Software doorbell (int 7): a device line was injected. Clear the latch first
    // (it has no peripheral source, so it would redeliver forever otherwise), then
    // service the recorded LOGICAL line. The bound handler masks/acks as needed
    // (tier-1 via irq_event_isr + irq_ack); do NOT mask here, or a tier-2 irq_attach
    // line stays masked forever after one delivery.
    if ((pending & (1u << SW_INT_L1)) != 0)
    {
        uint32_t bit = 1u << SW_INT_L1;
        __asm volatile("wsr.intclear %0; rsync" ::"a"(bit) : "memory");
        phys_int_disable(bit); // doorbell consumed: off until the next inject re-arms it
        int line = g_inject_line;
        g_inject_line = -1;
        if (line >= 0)
        {
            kickos_isr_irq(line);
        }
    }

    // Real device lines (chip-bound). Unlike the injected logical lines above there is no
    // doorbell and no g_irq_masked gating: the mask is the CPU interrupt's INTENABLE bit
    // and the clear is the driver's own peripheral register. A CPU interrupt several
    // routes share is demuxed ONCE.
    uint32_t served = 0;
    for (unsigned i = 0; i < LX6_DEV_ROUTES; i++)
    {
        int const ci = g_dev_cpu_int[i];
        if (ci < 0)
        {
            continue;
        }
        uint32_t const bit = 1u << static_cast<unsigned>(ci);
        if ((pending & bit) == 0 or (served & bit) != 0)
        {
            continue;
        }
        served = served | bit;
        kickos_lx6_dispatch_dev(ci);
    }
}

// --- Unhandled synchronous exception (startup.S _kickos_lx6_fault shim) --------
// The shim captured the fault special registers and re-enabled the window ABI; dump
// them, then hand off to the shared dead-end (halt on real HW, exit on a host/QEMU
// target). LEVEL1INTERRUPT (cause 4) is demuxed to the ISR entry before this path,
// so it never reaches the reporter.
void kickos_lx6_fault_report(uint32_t exccause, uint32_t excvaddr,
                             uint32_t epc1, uint32_t ps)
{
    kpanic_enter(); // mask IRQs + force the sync path + flush queued bytes, in order
    char const* what = "exception";
    if (exccause == 0)
    {
        what = "illegal instruction";
    }
    else if (exccause == 2)
    {
        what = "instruction fetch error";
    }
    else if (exccause == 3)
    {
        what = "load/store error";
    }
    else if (exccause == 6)
    {
        what = "integer divide by zero";
    }
    else if (exccause == 8)
    {
        what = "privileged instruction";
    }
    else if (exccause == 9)
    {
        what = "load/store alignment";
    }
    else if (exccause == 20)
    {
        what = "instruction fetch prohibited";
    }
    else if (exccause == 28)
    {
        what = "load prohibited";
    }
    else if (exccause == 29)
    {
        what = "store prohibited";
    }
    else if (exccause >= 32 and exccause <= 39)
    {
        what = "coprocessor disabled";
    }
    ::kickos::kprintf("\n=== XTENSA EXCEPTION (%s) ===\n", what);
#if KICKOS_PANIC_DUMP
    ::kickos::kprintf("  EXCCAUSE=0x%x EXCVADDR=0x%x\n", exccause, excvaddr);
    ::kickos::kprintf("  EPC1=0x%x PS=0x%x\n", epc1, ps);
#else
    (void)excvaddr;
    (void)epc1;
    (void)ps;
#endif
    kfault_terminate();
}

// --- Tickless clock (CCOUNT) + one-shot timer (CCOMPARE0) --------------------
// The CCOUNT read behind the arch_clock_now fallback TU (arch_clock_now_default.cc).
// The wrap-extend state lives here because kickos_lx6_init resets it, so the fallback
// cannot own it.
uint64_t kickos_lx6_ccount_ns(void)
{
    return cycles_to_ns(now_cycles());
}

// --- Trace clock (telemetry timestamp seam) ---------------------------------
// Raw 32-bit CCOUNT (wraps on its own; the host reconstructs absolute time from the
// SESSION clock_hz anchors). No ns conversion, no wrap-extend and no crit section, so it
// is safe to call from the switch path.
uint32_t arch_trace_now(void)
{
    return rd_ccount();
}

uint32_t arch_cpu_clock_hz(void)
{
    return SystemCoreClock;
}


// Smallest CCOMPARE margin worth attempting. Not a tuning knob: it only has to exceed
// the arm sequence's own latency so the common case arms first try, and the loop below
// is what makes correctness independent of it.
static constexpr uint32_t CCOMPARE_MIN_CYCLES = 64;

void arch_timer_arm(uint64_t deadline_ns)
{
    uint64_t now = arch_clock_now();
    uint64_t delta_ns = 0;
    if (deadline_ns > now)
    {
        delta_ns = deadline_ns - now;
    }
    // Clamp the delta to the 32-bit CCOMPARE range BEFORE converting so a far-
    // future deadline can't overflow the ns*freq product. A clamped deadline fires
    // early and the kernel re-arms the remainder (a harmless extra wake).
    uint64_t f = SystemCoreClock;
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
    // CCOMPARE0 is an EQUALITY match against a free-running CCOUNT, not a countdown to
    // zero: a compare value already BEHIND CCOUNT when it lands is not late, it is missed,
    // and the next match is a full 2^32-cycle wrap away (about 18 s at 240 MHz), which
    // presents as a hang rather than as jitter. The handful of instructions between
    // reading CCOUNT and writing CCOMPARE is enough to lose a one-cycle margin, so a
    // floor of 1 is not a floor at all.
    //
    // Arming is therefore a LOOP that CHECKS: write the compare, then ask whether CCOUNT
    // has already passed it, and widen the margin and retry if so. The signed difference
    // is what makes the comparison wrap-correct.
    uint32_t margin = static_cast<uint32_t>(cyc);
    if (margin < CCOMPARE_MIN_CYCLES)
    {
        margin = CCOMPARE_MIN_CYCLES;
    }
    while (true)
    {
        uint32_t const cmp = rd_ccount() + margin;
        __asm volatile("wsr.ccompare0 %0; rsync" ::"a"(cmp) : "memory");
        if (static_cast<int32_t>(rd_ccount() - cmp) < 0)
        {
            break; // the compare is still ahead of the counter: genuinely armed
        }
        if (margin > (0xFFFFFFFFu / 2u))
        {
            break; // cannot widen further; the wrap is the deadline either way
        }
        margin = margin * 2u;
    }
    phys_int_enable(1u << CCOMPARE0_INT);
}

void arch_timer_disarm(void)
{
    // Disable the physical timer line; the pending CCOMPARE0 match is cleared by the
    // next wsr.ccompare0 (arch_timer_arm). No callback fires while disabled.
    phys_int_disable(1u << CCOMPARE0_INT);
}

// --- MPU: no hardware per-task protection on the classic ESP32 ---------------
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n)
{
    (void)regions;
    (void)n;
}

// LX6 has neither an MPU nor a ring split. The symbol must still resolve, as the
// self-grant path calls it.
void kickos_arch_mpu_commit(void) {}

// No per-task MPU on the classic ESP32 and no privilege split: 0 keeps arch_ram_alloc
// byte-granular, since region shaping would waste RAM with nothing to enforce.
size_t arch_mpu_min_region(void)
{
    return 0u;
}

// No hardware MPU: no descriptor to satisfy, so a nonzero 16-byte-aligned window is
// "encodable" (arch_mpu_apply is a no-op here, nothing is actually enforced).
bool arch_mpu_region_encodable(uintptr_t base, size_t size)
{
    if (size == 0)
    {
        return false;
    }
    return (base & 15u) == 0 and (size & 15u) == 0;
}

// Never read: arch_mpu_min_region() is 0 here, which short-circuits both inlines before
// the mode is reached. Defined only so the symbol resolves in an LX6 link.
int arch_mpu_region_pow2(void)
{
    return 1;
}



// --- The kernel-owned mask for a REAL device line (RULE L1) -------------------
// Clears / sets the INTENABLE bit of the CPU interrupt the matrix drives the device to.
// INTENABLE is core state reached only by rsr/wsr, so it can never be delegated to a
// driver; the driver's own UART_INT_ENA is off limits to the kernel.
// A line with no device route is an injected software line: no-op.
// The INTENABLE read-modify-write is atomic only because phys_int_* raises PS.INTLEVEL
// around it, and the rsync closing that window is what serializes the write; a bare
// wsr.intenable here would be neither.
void kickos_lx6_hw_mask(int line)
{
    int const ci = dev_route_cpu_int(line);
    if (ci < 0)
    {
        return;
    }
    phys_int_disable(1u << static_cast<unsigned>(ci));
}

void kickos_lx6_hw_unmask(int line)
{
    int const ci = dev_route_cpu_int(line);
    if (ci < 0)
    {
        return;
    }
    phys_int_enable(1u << static_cast<unsigned>(ci));
}

// --- Interrupt controller: a SOFTWARE controller over the logical device lines ---
// Xtensa INTSET latches only the software-type interrupts (int 7/29), so an INJECTED
// logical line cannot be a physical INTENABLE bit: lines 5/9/11 would be silent no-ops
// and line 6 collides with the timer. Mask is a software bitmask and inject records the
// line then rings the ONE real software int 7 (dispatched in kickos_lx6_dispatch_l1).
// A line with a device route additionally reaches INTENABLE: a LEVEL peripheral source
// keeps re-asserting until the controller masks it, so the software bit alone would
// livelock the level-1 handler.
void arch_irq_mask(int line)
{
    if (line < 0)
    {
        return;
    }
    arch_irq_state_t s = arch_irq_save();
    g_irq_masked = g_irq_masked | (1u << (static_cast<unsigned>(line) & 31u));
    kickos_lx6_hw_mask(line);
    arch_irq_restore(s);
}

void arch_irq_unmask(int line)
{
    if (line < 0)
    {
        return;
    }
    unsigned l = static_cast<unsigned>(line) & 31u;
    arch_irq_state_t s = arch_irq_save();
    g_irq_masked &= ~(1u << l);
    kickos_lx6_hw_unmask(line);
    // Latch-and-coalesce: a raise taken while this line was masked redelivers now
    // through the int-7 doorbell, on the normal ISR path rather than as a direct post.
    if ((g_irq_pending & (1u << l)) != 0)
    {
        g_irq_pending &= ~(1u << l);
        g_inject_line = static_cast<int>(l);
        uint32_t bit = 1u << SW_INT_L1;
        phys_int_enable(bit);
        __asm volatile("wsr.intset %0; rsync" ::"a"(bit) : "memory");
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
    g_irq_pending = g_irq_pending & ~(1u << (static_cast<unsigned>(line) & 31u));
    arch_irq_restore(s);
}

void arch_irq_inject(int irq)
{
    if (irq < 0)
    {
        return;
    }
    // Bracketed like arch_irq_mask/unmask: an ISR reaching those read-modify-writes the
    // same words.
    arch_irq_state_t s = arch_irq_save();
    // Latch-and-coalesce: a raise on a masked line latches one-deep (redelivered at
    // unmask), it is NOT dropped.
    if ((g_irq_masked & (1u << (static_cast<unsigned>(irq) & 31u))) != 0)
    {
        g_irq_pending = g_irq_pending | (1u << (static_cast<unsigned>(irq) & 31u));
    }
    else
    {
        // recorded BEFORE ringing the doorbell (the dispatcher reads it)
        g_inject_line = irq;
        uint32_t bit = 1u << SW_INT_L1;
        // Enable the doorbell JUST-IN-TIME (the dispatcher disables it again). Leaving it
        // enabled at rest storms the level-1 handler: the ROM boots with int 7 pending.
        phys_int_enable(bit);
        __asm volatile("wsr.intset %0; rsync" ::"a"(bit) : "memory");
    }
    arch_irq_restore(s);
}

// --- Device-route bind (chip layer) -----------------------------------------
// Adds one (CPU interrupt, logical line) route and arms that CPU interrupt in INTENABLE.
// Several lines may name the same cpu_int (the grouped-line shape).
// Call ONLY from arch_init, before any interrupt is enabled: the route arrays are read in
// ISR context and written here with no critical section, and the arming write is what
// publishes them. A route past LX6_DEV_ROUTES is dropped silently.
void kickos_lx6_bind_dev_int(int cpu_int, int line)
{
    if (cpu_int < 0 or cpu_int > 31 or line < 0)
    {
        return;
    }
    for (unsigned i = 0; i < LX6_DEV_ROUTES; i++)
    {
        if (g_dev_cpu_int[i] >= 0)
        {
            continue;
        }
        g_dev_line[i] = static_cast<int8_t>(static_cast<unsigned>(line) & 31u);
        g_dev_cpu_int[i] = static_cast<int8_t>(cpu_int);
        phys_int_enable(1u << static_cast<unsigned>(cpu_int));
        return;
    }
}

// --- Idle -------------------------------------------------------------------
void arch_idle_wait(void)
{
    __asm volatile("waiti 0"); // wait for interrupt at level 0 (WFI twin)
}

// --- Syscall: a plain call, since there is no CPU ring split and so no trap ---
// The contract is satisfied trivially: dispatch already runs privileged (the only
// mode), in thread context, on the caller's stack; arch_in_isr() reads false; a
// blocking syscall blocks by an ordinary synchronous arch_switch.
// The windowed ABI is not a concern here: with no trap, both entry points are
// ordinary calls, so the compiler places the 64-bit result in the caller's window
// exactly as it does for any long long.
uint64_t arch_syscall64(uintptr_t nr,
                        uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    return syscall_dispatch(nr, a0, a1, a2, a3);
}

uintptr_t arch_syscall(uintptr_t nr,
                       uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    return static_cast<uintptr_t>(syscall_dispatch(nr, a0, a1, a2, a3));
}

// --- One-time core bring-up, called by the chip's arch_init -----------------
void kickos_lx6_init(void)
{
    // Every physical line masked. The timer enables CCOMPARE0 on arm; the int-7
    // software doorbell is enabled JUST-IN-TIME by arch_irq_inject and disabled again
    // by the dispatcher, since leaving it on at rest storms the level-1 handler (the
    // ROM boots with int 7 already pending). Logical device lines are gated by
    // g_irq_masked, not INTENABLE.
    wr_intenable(0);
    // Enable coprocessor 0 (the single-precision FPU) for every thread, so `float`
    // works uniformly across the board fleet. CPENABLE is global (not per-thread):
    // the switch banks the FP data registers, not this enable. FP regs are caller-
    // saved, so only the preemptive path (the level-1 interrupt frame, startup.S)
    // saves f0-f15+FCR+FSR; the cooperative switch relies on the compiler's spill.
    __asm volatile("wsr.cpenable %0; rsync" ::"a"(1u) : "memory"); // CP0 bit0
    g_cyc_high = 0;
    g_cyc_last = 0;
}

}
