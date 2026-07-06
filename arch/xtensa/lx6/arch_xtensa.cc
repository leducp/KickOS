// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Xtensa LX6 arch backend: the ISA-generic half of the arch.h seam for the
// classic ESP32 (WINDOWED ABI). The context switch + first-thread entry + fresh-
// thread trampoline assembly is in switch.S; the chip layer (arch/xtensa/chip/
// esp32) supplies arch_init / arch_console_write / arch_shutdown + the startup
// vectors (incl. the mandatory window over/underflow handlers + the level-1
// interrupt entry) + linker script + SystemCoreClock. Clean-room: Xtensa special
// registers per the Xtensa ISA reference; ESP32 interrupt numbers per the TRM.
//
// M1.x scope (build-only, HW deferred): the cooperative (thread-context) switch,
// the RSIL critical section, the CCOUNT/CCOMPARE0 timer, the plain-call syscall,
// the level-1 interrupt dispatch (kickos_lx6_dispatch_l1 -> kickos_isr_timer /
// kickos_isr_irq), AND the preemptive switch-on-ISR-exit are complete. A switch
// requested from ISR context sets g_arch_switch_pending; the level-1 interrupt
// exit (startup.S _kickos_int_level1) completes the physical swap, saving the
// preempted thread in the interrupt-frame format and resuming the target by the
// path its resume_kind selects (retw for a cooperatively-blocked thread, rfe for
// one previously preempted). There is no privilege split on this core, so MPU/
// privilege are no-ops.

#include <kickos/arch/arch.h>
#include <kickos/units.h> // _s literal (== 1e9 ns) for the cycle<->ns conversions

#include <stddef.h> // offsetof

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
    // PS fields (Xtensa ISA / corebits.h): UM=bit5, WOE=bit18. A resumed thread
    // runs windowed (WOE=1) with interrupts enabled (INTLEVEL=0).
    constexpr uint32_t PS_UM = 0x20;
    constexpr uint32_t PS_WOE = 0x40000;

    // A call4 return address carries CALLINC=1 in bits [31:30]; the fresh-thread
    // resume (arch_start) uses it so the first retw takes the 4-register underflow
    // path into the trampoline (switch.S). The low 30 bits are the target; the top
    // 2 bits of the live PC supply the region, so masking to 30 bits is safe here
    // (the whole image links inside one 1 GiB region).
    constexpr uint32_t PC_CALL4 = 0x1u << 30;

    // Critical-section interrupt level: mask the C-handleable levels 1-3 (the timer
    // + all device lines), leaving the high-level 4-7 / NMI zero-latency band
    // unmaskable -- the BASEPRI-band analog.
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

    // DWT-analog wrap extension of the 32-bit CCOUNT cycle counter.
    // LIMITATION (M1): at 240 MHz CCOUNT wraps every ~17.9 s; a wrap not observed
    // within one 2^32-cycle period is missed. A TIMG 64-bit source is the
    // refinement the spike names.
    volatile uint32_t g_cyc_high = 0;
    volatile uint32_t g_cyc_last = 0;

    // User-RAM bump allocator (arch_ram_alloc), bounds from the chip linker script.
    volatile uint32_t g_ram_used = 0;

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
}

extern "C"
{
    // Linker-provided user-RAM bounds (chip linker script).
    extern unsigned char __kickos_ram_start[];
    extern unsigned char __kickos_ram_end[];

    // The core clock in Hz, defined + maintained by the chip backend.
    extern uint32_t SystemCoreClock;

    // The windowed cooperative swap + the fresh-thread trampoline (switch.S).
    void xtensa_switch(struct arch_context* from, struct arch_context* to);
    void _thread_trampoline(void);

    // Shared with switch.S/arch_start/startup.S: the ctx of the running thread, and
    // the deferred-switch target when arch_switch is called from ISR context.
    // volatile: written by C and by asm across a seam the compiler cannot see.
    struct arch_context* volatile g_arch_current = nullptr;
    struct arch_context* volatile g_arch_next = nullptr;

    // Set by arch_switch when it defers a switch from ISR context; consumed (and
    // cleared) by the level-1 interrupt exit (startup.S _kickos_int_level1), which
    // completes the swap on the way back to thread level. Distinct from comparing
    // g_arch_current vs g_arch_next: the cooperative path advances g_arch_current
    // without touching g_arch_next, so a stale g_arch_next must NOT be mistaken for
    // a pending preemption -- this flag is the unambiguous request. u32 for a
    // trivial asm load/store.
    volatile uint32_t g_arch_switch_pending = 0;

    // In-ISR depth (the IPSR!=0 analog). Maintained by the level-1 interrupt entry
    // (startup.S). arch_in_isr() reads it; the kernel uses it to forbid blocking
    // from ISR context and to defer a switch to interrupt exit.
    volatile uint32_t g_isr_depth = 0;
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
            g_cyc_high = g_cyc_high + 1; // plain read+write (avoid deprecated volatile ++)
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

// --- Context init: fabricate a first-resume frame for switch.S/arch_start ----
// The windowed resume rebuilds the incoming call chain from the thread's stack
// via the window-underflow vector. So the fabricated stack must look exactly like
// a thread that suspended inside xtensa_switch after being reached from the
// trampoline by a call4: the trampoline's a0-a3 sit in the base save area below
// the (fabricated) switch frame, and ctx.pc is the trampoline encoded as a call4
// return address.
void arch_context_init(struct arch_context* ctx,
                       void (*entry)(void*), void* arg,
                       void* stack_base, size_t stack_size,
                       int privileged)
{
    (void)privileged; // no privilege split on this core

    uintptr_t top = reinterpret_cast<uintptr_t>(stack_base) + stack_size;
    top &= ~static_cast<uintptr_t>(15);        // 16-byte stack alignment (ABI)

    uint32_t sp_tramp = static_cast<uint32_t>(top);   // trampoline frame (outermost)
    uint32_t sp_sw = sp_tramp - 32;                   // fabricated xtensa_switch frame

    // The first retw (call4) underflows the trampoline's window from the base save
    // area at [sp_sw - 16 .. sp_sw - 4] (Xtensa base-save-area convention).
    uint32_t* save = reinterpret_cast<uint32_t*>(static_cast<uintptr_t>(sp_sw - 16));
    save[0] = 0;                                        // a0: trampoline never retw's
    save[1] = sp_tramp;                                 // a1: trampoline SP
    save[2] = reinterpret_cast<uint32_t>(entry);        // a2: entry
    save[3] = reinterpret_cast<uint32_t>(arg);          // a3: arg

    ctx->sp = sp_sw;
    ctx->ps = PS_WOE | PS_UM;                           // windowed, ints enabled, INTLEVEL 0
    ctx->pc = PC_CALL4
              | (reinterpret_cast<uint32_t>(_thread_trampoline) & 0x3FFFFFFFu);
    ctx->resume_kind = KICKOS_RESUME_COOP;              // enters via the trampoline retw
}

// --- Switch: synchronous in thread context; deferred in ISR context ----------
void arch_switch(struct arch_context* from, struct arch_context* to)
{
    if (g_isr_depth != 0)
    {
        // Preemption: record the target and flag the request. The physical swap is
        // completed at level-1 interrupt exit (startup.S _kickos_int_level1), which
        // saves the interruptee (g_arch_current) in the interrupt-frame format and
        // resumes `to` -- the deferred-switch analog of ARM PendSV / RX SWINT.
        g_arch_next = to;
        g_arch_switch_pending = 1;
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

    // Timer (CCOMPARE0). Mask stops re-fire until the kernel re-arms (which clears
    // the compare match); the pending bit is level-triggered off the comparator.
    if ((pending & (1u << CCOMPARE0_INT)) != 0)
    {
        arch_irq_mask(CCOMPARE0_INT);
        kickos_isr_timer();
        pending &= ~(1u << CCOMPARE0_INT);
    }

    // Device lines: mask before waking the driver (thread context), which re-unmasks
    // via irq_ack once serviced -- the line cannot re-fire while being handled.
    while (pending != 0)
    {
        int line = __builtin_ctz(pending);
        arch_irq_mask(line);
        if (line == SW_INT_L1)
        {
            // Software-latched line: clear the pending bit before servicing (a real
            // external/level line clears at its source instead).
            uint32_t bit = 1u << static_cast<unsigned>(SW_INT_L1);
            __asm volatile("wsr.intclear %0; rsync" ::"a"(bit) : "memory");
        }
        kickos_isr_irq(line);
        pending &= ~(1u << static_cast<unsigned>(line));
    }
}

// --- Tickless clock (CCOUNT) + one-shot timer (CCOMPARE0) --------------------
// weak: the monotonic clock SOURCE may be overridden by a chip that prefers a
// 64-bit TIMG timer (avoids the CCOUNT 32-bit wrap); CCOUNT is the default.
uint64_t __attribute__((weak)) arch_clock_now(void)
{
    return cycles_to_ns(now_cycles());
}

// --- Trace clock (telemetry timestamp seam) ---------------------------------
// Raw 32-bit CCOUNT: the cycle counter is the ideal cycle-accurate trace source
// (u32, wraps on its own; the host reconstructs absolute time from the SESSION
// clock_hz anchors). Read raw -- no ns conversion, no wrap-extend, no crit
// section -- so it is safe to call from the switch path. KICKOS_HAVE_TRACE_CLOCK
// is set for lx6.
uint32_t arch_trace_now(void)
{
    return rd_ccount();
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
    if (cyc == 0)
    {
        cyc = 1; // never program the current instant
    }
    uint32_t cmp = rd_ccount() + static_cast<uint32_t>(cyc);
    __asm volatile("wsr.ccompare0 %0; rsync" ::"a"(cmp) : "memory");
    arch_irq_unmask(CCOMPARE0_INT);
}

void arch_timer_disarm(void)
{
    // Mask the timer line; the pending CCOMPARE0 match is cleared by the next
    // wsr.ccompare0 (arch_timer_arm). No callback fires while masked.
    arch_irq_mask(CCOMPARE0_INT);
}

// --- MPU: no hardware per-task protection on the classic ESP32 ---------------
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n)
{
    (void)regions;
    (void)n;
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
    size_t need = (size + 15u) & ~static_cast<size_t>(15u); // 16-byte aligned
    arch_irq_state_t s = arch_irq_save();
    void* p = nullptr;
    if (need != 0 and need <= total - g_ram_used)
    {
        p = __kickos_ram_start + g_ram_used;
        g_ram_used = g_ram_used + static_cast<uint32_t>(need);
    }
    arch_irq_restore(s);
    return p;
}

uintptr_t arch_mpu_probe_addr(void)
{
    return 0; // no enforced MPU -> no unprivileged-access fault to probe
}

// --- Interrupt controller (INTENABLE mask + INTSET software raise) -----------
void arch_irq_mask(int line)
{
    if (line < 0)
    {
        return;
    }
    uint32_t bit = 1u << (static_cast<unsigned>(line) & 31u);
    arch_irq_state_t s = arch_irq_save();
    wr_intenable(rd_intenable() & ~bit);
    arch_irq_restore(s);
}

void arch_irq_unmask(int line)
{
    if (line < 0)
    {
        return;
    }
    uint32_t bit = 1u << (static_cast<unsigned>(line) & 31u);
    arch_irq_state_t s = arch_irq_save();
    wr_intenable(rd_intenable() | bit);
    arch_irq_restore(s);
}

void arch_irq_inject(int irq)
{
    if (irq < 0)
    {
        return;
    }
    uint32_t bit = 1u << (static_cast<unsigned>(irq) & 31u);
    // Match the proven sim/ARM semantics: a raise on a masked line is dropped.
    // Only the ESP32 software-interrupt lines (internal 7 @ L1, 29 @ L3) actually
    // latch via INTSET; a hardware line ignores it (test scaffolding only).
    if ((rd_intenable() & bit) == 0)
    {
        return;
    }
    __asm volatile("wsr.intset %0; rsync" ::"a"(bit) : "memory");
}

// --- Idle -------------------------------------------------------------------
void arch_idle_wait(void)
{
    __asm volatile("waiti 0"); // wait for interrupt at level 0 (WFI twin)
}

// --- Syscall: a plain call -- no CPU ring split, so no trap ------------------
// The contract is satisfied trivially: dispatch already runs privileged (the only
// mode), in thread context, on the caller's stack; arch_in_isr() reads false; a
// blocking syscall blocks by an ordinary synchronous arch_switch. Exactly the sim.
uintptr_t arch_syscall(uintptr_t nr,
                       uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    return syscall_dispatch(nr, a0, a1, a2, a3);
}

// --- One-time core bring-up, called by the chip's arch_init -----------------
void kickos_lx6_init(void)
{
    wr_intenable(0); // start with every line masked; drivers unmask as needed
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
