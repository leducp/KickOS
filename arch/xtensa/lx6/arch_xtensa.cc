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

// Fault reporting (see the _kickos_lx6_fault shim in startup.S, which captures the
// fault special registers and calls here with the window ABI live): the reporter
// calls kpanic_enter first, which masks IRQs, forces the synchronous polled writer,
// and flushes the ring. This is load-bearing on ESP32: the shim runs at INTLEVEL=15
// with g_isr_depth still 0, so without forcing the sync path the dump would enqueue
// into the buffered UART0 ring whose drain interrupt is masked -- and be lost.
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

    // PHYSICAL interrupt enable/disable (INTENABLE), for the timer (CCOMPARE0) and
    // the int-7 software doorbell -- NOT the public arch_irq_* seam, which is the
    // software controller over LOGICAL device lines.
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

    // Software interrupt controller for the LOGICAL device lines (arch.h inject/
    // mask contract), decoupled from the physical Xtensa interrupts. Xtensa INTSET
    // only latches the software-type lines (int 7/29), so a logical line can't be a
    // physical bit; instead a raise records the line here and rings the ONE real
    // software doorbell (int 7), whose dispatcher services this line -- exactly the
    // RISC-V SSIP / host-sim SIGUSR1 model. The timer (CCOMPARE0) and the int-7
    // doorbell are the only PHYSICAL lines, driven via INTENABLE directly.
    // logical line masked bitmask (1 = masked). All lines start MASKED at reset
    // (the arch.h reset contract, matching the NVIC/RX silicon posture); a driver
    // unmasks its line (arch_irq_unmask, or irq_register) before use.
    static volatile uint32_t g_irq_masked = 0xFFFFFFFFu;
    static volatile int g_inject_line = -1;    // pending software-injected logical line

    // A PHYSICAL device interrupt bound by the chip (esp32 UART0 TX-empty -> the
    // buffered console drain). The interrupt matrix routes the peripheral source to
    // CPU interrupt g_console_cpu_int (armed in INTENABLE by kickos_lx6_bind_console_
    // int); when it fires, the level-1 dispatcher runs the ISR attached to the logical
    // line g_console_line via irq_attach -- a real hardware line like CCOMPARE0, NOT a
    // software-injected logical line (no doorbell, no g_irq_masked gating). -1 until
    // the chip binds it.
    static volatile int g_console_cpu_int = -1;
    static volatile int g_console_line = -1;
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
    // line stays masked forever after one delivery. Matches sim/riscv/rx.
    if ((pending & (1u << SW_INT_L1)) != 0)
    {
        uint32_t bit = 1u << static_cast<unsigned>(SW_INT_L1);
        __asm volatile("wsr.intclear %0; rsync" ::"a"(bit) : "memory");
        phys_int_disable(bit); // doorbell consumed: off until the next inject re-arms it
        int line = g_inject_line;
        g_inject_line = -1;
        if (line >= 0)
        {
            kickos_isr_irq(line);
        }
    }

    // UART0 TX-empty (chip-bound via kickos_lx6_bind_console_int): a PHYSICAL line
    // the interrupt matrix routes to g_console_cpu_int. It has a real peripheral
    // source, so -- unlike the injected logical lines above -- there is no doorbell /
    // software-mask dance: run the attached drain ISR (console_tx_isr), which clears
    // the source by dropping the UART TX-empty enable when the ring empties (a level
    // source gated at the peripheral, so no wsr.intclear here).
    if (g_console_cpu_int >= 0
        and (pending & (1u << static_cast<unsigned>(g_console_cpu_int))) != 0)
    {
        kickos_isr_irq(g_console_line);
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

// No per-task MPU on the classic ESP32 (no privilege split either); the pow2
// shaping is inert here, kept only so arch_ram_alloc's contract is uniform.
size_t arch_mpu_min_region(void)
{
    return 32u;
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
    if (size == 0)
    {
        return nullptr;
    }
    size_t const rsz = arch_ram_region_size(size); // pow2, naturally alignable
    size_t const total = arch_ram_size();
    uintptr_t const base = reinterpret_cast<uintptr_t>(__kickos_ram_start);
    arch_irq_state_t s = arch_irq_save();
    void* p = nullptr;
    uintptr_t const cur = base + g_ram_used;
    uintptr_t const aligned = (cur + (rsz - 1)) & ~static_cast<uintptr_t>(rsz - 1);
    size_t const off = static_cast<size_t>(aligned - base);
    if (aligned >= cur and off <= total and rsz <= total - off)
    {
        p = reinterpret_cast<void*>(aligned);
        g_ram_used = static_cast<uint32_t>(off + rsz);
    }
    arch_irq_restore(s);
    return p;
}

uintptr_t arch_mpu_probe_addr(void)
{
    return 0; // no enforced MPU -> no unprivileged-access fault to probe
}

// --- Interrupt controller: a SOFTWARE controller over the logical device lines --
// Xtensa INTSET latches only the software-type interrupts (int 7/29), so a logical
// line cannot be a physical INTENABLE bit (the old code assumed it could -- lines
// 5/9/11 were silent no-ops, and line 6 collided with the timer). Instead mask is a
// software bitmask and inject records the line + rings the ONE real software int 7
// (dispatched in kickos_lx6_dispatch_l1). Mirrors the RISC-V SSIP / host-sim model.
void arch_irq_mask(int line)
{
    if (line < 0)
    {
        return;
    }
    arch_irq_state_t s = arch_irq_save();
    g_irq_masked = g_irq_masked | (1u << (static_cast<unsigned>(line) & 31u));
    arch_irq_restore(s);
}

void arch_irq_unmask(int line)
{
    if (line < 0)
    {
        return;
    }
    arch_irq_state_t s = arch_irq_save();
    g_irq_masked = g_irq_masked & ~(1u << (static_cast<unsigned>(line) & 31u));
    arch_irq_restore(s);
}

void arch_irq_inject(int irq)
{
    if (irq < 0)
    {
        return;
    }
    // A raise on a masked line is dropped (the proven sim/ARM/RISC-V semantics).
    if ((g_irq_masked & (1u << (static_cast<unsigned>(irq) & 31u))) != 0)
    {
        return;
    }
    g_inject_line = irq; // recorded BEFORE ringing the doorbell (the dispatcher reads it)
    uint32_t bit = 1u << static_cast<unsigned>(SW_INT_L1);
    // Enable the doorbell JUST-IN-TIME (the dispatcher disables it again). Leaving it
    // enabled at rest storms the level-1 handler -- the ROM boots with int 7 pending.
    phys_int_enable(bit);
    __asm volatile("wsr.intset %0; rsync" ::"a"(bit) : "memory");
}

// --- Physical device-interrupt bind (chip layer) ----------------------------
// Records the CPU interrupt the matrix routes a device to + the logical line its
// ISR is attached to, and arms that CPU interrupt in INTENABLE. Used by the esp32
// chip for the UART0 TX-empty -> console-drain path. Distinct from arch_irq_* (the
// SOFTWARE controller over injected logical lines): this only unmasks the physical
// CPU line, once; the per-transfer gate stays at the peripheral (the console
// backend's irq_enable/irq_disable). Leaves g_irq_masked and the int-7 doorbell
// untouched.
void kickos_lx6_bind_console_int(int cpu_int, int line)
{
    if (cpu_int < 0 or line < 0)
    {
        return;
    }
    g_console_cpu_int = cpu_int;
    g_console_line = line;
    phys_int_enable(1u << static_cast<unsigned>(cpu_int));
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
    // Every physical line masked. The timer enables CCOMPARE0 on arm; the int-7
    // software doorbell is enabled JUST-IN-TIME by arch_irq_inject and disabled
    // again by the dispatcher -- leaving it on at rest storms the level-1 handler
    // (the ROM boots with int 7 already pending). Logical device lines are gated by
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
