// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
#include <kickos/arch/arch.h>
#include <kickos/arch/xtensa_frame.h> // F_* interrupt-frame offsets, shared with startup.S
#include <kickos/units.h> // _s == 1e9 ns
#include <kickos/trace/record.h>
#include <kickos/sys/atomic.h>

#include <stddef.h> // offsetof

// A KICKOS_TRACE_ARCH (CMake ladder / this chip's caps.cmake) that does not name this
// backend's arch mislabels every SESSION record.
static_assert(KICKOS_TRACE_ARCH == kickos::trace::ARCH_XTENSA,
              "KICKOS_TRACE_ARCH does not match ArchId::ARCH_XTENSA for lx6");

// The _kickos_lx6_fault shim (startup.S) runs at INTLEVEL=15 with g_isr_depth still 0, so
// the reporter must call kpanic_enter first: without the forced synchronous writer the dump
// enqueues into the buffered UART0 ring whose drain interrupt is masked, and is lost.
namespace kickos
{
    void kprintf(char const* fmt, ...);
}
extern "C" void kpanic_enter(void);
extern "C" void kfault_terminate(void) __attribute__((noreturn));

// -DKICKOS_PANIC_DUMP=0 keeps only the one-line fault marker.
#ifndef KICKOS_PANIC_DUMP
#define KICKOS_PANIC_DUMP 1
#endif

// switch.S + startup.S hard-code these arch_context field offsets.
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
    // WOE=bit18.
    constexpr uint32_t PS_UM = 0x20;
    constexpr uint32_t PS_EXCM = 0x10;
    constexpr uint32_t PS_WOE = 0x40000;
    constexpr uint32_t PS_CALLINC1 = 0x1u << 16; // CALLINC=1

    inline uint32_t f_areg(unsigned n) { return F_AREG(n); }

    // Masks the C-handleable levels 1-3 (the timer + all device lines); the high-level
    // 4-7 / NMI zero-latency band stays unmaskable.
    constexpr uint32_t KICKOS_IRQ_LOCK_LEVEL = 3;

    // ESP32 CCOMPARE0 (Xtensa timer 0) is wired to per-CPU internal interrupt 6, a
    // level-1 (C-handleable) line (ESP32 TRM 4.3, "CPU interrupts" table).
    constexpr int CCOMPARE0_INT = 6;

    // ESP32 internal interrupt 7 is the level-1 SOFTWARE interrupt: the only L1 line INTSET
    // can latch. It has no peripheral source, so the dispatcher must wsr.intclear it or one
    // inject redelivers forever.
    constexpr int SW_INT_L1 = 7;

    // XCHAL_INTLEVEL1_MASK for the LX6 config: only these lines reach the level-1 entry;
    // higher-level lines have their own vectors.
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

    // PHYSICAL enable/disable (INTENABLE). NOT the public arch_irq_* seam, which is the
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

    // Real device lines: the matrix routes a peripheral source to a CPU interrupt, and the
    // chip demuxes that CPU interrupt into logical lines. INTENABLE is their kernel-owned
    // mask (RULE L1); the peripheral's own enable register belongs to the driver.
    // The mask is COARSE: several lines may name the same cpu_int, and masking one of them
    // masks its siblings.
    constexpr unsigned LX6_DEV_ROUTES = 4;
    Atomic<int8_t, Order::RELAXED> g_dev_cpu_int[LX6_DEV_ROUTES] = {-1, -1, -1, -1};
    Atomic<int8_t, Order::RELAXED> g_dev_line[LX6_DEV_ROUTES] = {-1, -1, -1, -1};

    // The CPU interrupt a logical line's mask targets, or -1 for a line with no route
    // (an injected software line, whose mask is the g_irq_masked bit alone).
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
    // (0..N logical lines), and owns the per-source clear discipline. No _default.cc
    // fallback: a chip that binds a device route without defining this fails to LINK.
    void kickos_lx6_dispatch_dev(int cpu_int);

    // Shared with switch.S/arch_start/startup.S, written by C and by asm: the ctx of the
    // running thread, and the deferred-switch target when arch_switch runs in ISR context.
    kickos::Atomic<struct arch_context*, kickos::Order::RELAXED> g_arch_current = nullptr;
    kickos::Atomic<struct arch_context*, kickos::Order::RELAXED> g_arch_next = nullptr;

    // switch.S and the chip startup.S load each as a plain word at offset 0.
    static_assert(sizeof(g_arch_current) == sizeof(struct arch_context*), "asm reads one word");
    static_assert(sizeof(g_arch_next) == sizeof(struct arch_context*), "asm reads one word");
    static_assert(alignof(decltype(g_arch_current)) == alignof(struct arch_context*), "asm reads it naturally aligned");

    // Set by arch_switch when it defers a switch from ISR context; consumed and cleared by
    // the level-1 interrupt exit (startup.S _kickos_int_level1). NOT derivable from
    // g_arch_current != g_arch_next: the cooperative path advances g_arch_current without
    // touching g_arch_next, so a stale g_arch_next is not a pending preemption.
    kickos::Atomic<uint32_t, kickos::Order::RELAXED> g_arch_switch_pending = 0;

    // In-ISR depth (the IPSR!=0 analog), maintained by the level-1 interrupt entry
    // (startup.S).
    uint32_t g_isr_depth = 0;

    // startup.S carries both with l32i/s32i at offset 0.
    static_assert(sizeof(g_arch_switch_pending) == 4, "asm reads one word");
    static_assert(sizeof(g_isr_depth) == 4, "asm reads one word");

    // Software interrupt controller for the LOGICAL device lines (arch.h inject/mask
    // contract). Xtensa INTSET only latches the software-type lines (int 7/29), so a
    // logical line cannot be a physical bit: a raise records the line here and rings the
    // ONE real software doorbell (int 7). The timer (CCOMPARE0) and that doorbell are the
    // only PHYSICAL lines.
    // 1 = masked. All lines start MASKED at reset (the arch.h reset contract).
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
        // The wrap-extend read must be atomic against a concurrent reader (thread + ISR).
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
// A fresh thread starts via the SAME rfe restore path as a preempted one
// (_kickos_lx6_irq_restore, startup.S), never a fabricated `retw` underflow: a retw start
// leaves the trampoline a phantom windowed frame with no valid base-save-area caller
// linkage, and a non-blocking entry->run->exit wraps WindowBase around the whole 64-AR
// file, overflowing that frame through the invalid linkage into data RAM.
//
// `entry` rotates the window by PS.CALLINC and, for a call4 frame, maps the caller's a6/a7
// to the callee's a2/a3 (Xtensa ISA, Windowed Register Option).
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
    // EXCM keeps the restore's PS write from being interrupted mid-sequence; rfe clears
    // it. CALLINC=1 opens a CALL4 frame. INTLEVEL=0: the thread runs interrupts enabled.
    set(F_PS, PS_UM | PS_WOE | PS_EXCM | PS_CALLINC1);
    set(f_areg(1), static_cast<uint32_t>(top));         // a1 = stack top (entry's input SP)
    set(f_areg(6), reinterpret_cast<uint32_t>(entry));  // a6 -> trampoline a2 (entry fn)
    set(f_areg(7), reinterpret_cast<uint32_t>(arg));    // a7 -> trampoline a3 (arg)
    // a0/a4 already 0: a0 is discarded by the entry rotation, and a4 becomes the
    // trampoline's a0 (its return address), where 0 is the safe outermost value.

    ctx->sp = frame_base;                               // base of the interrupt frame
    ctx->ps = 0;                                        // unused for an IRQ-resumed thread
    ctx->pc = 0;                                        // (PS/PC live in the frame)
    ctx->resume_kind = KICKOS_RESUME_IRQ;              // enters via rfe (irq_restore)
}

// The redirected thread may have been saved as KICKOS_RESUME_COOP; arch_context_init must
// overwrite resume_kind, or the switcher takes the retw path onto an INTERRUPT frame.
//
// `privileged` is discarded on this core: PS.UM is 1 for kernel and thread alike.
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
        // The physical swap happens at level-1 interrupt exit (startup.S
        // _kickos_int_level1), which saves g_arch_current in the interrupt-frame format.
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
    // RSIL atomically returns the old PS and raises PS.INTLEVEL. Nesting-safe: the whole
    // PS is saved/restored, so a prior raised level is preserved.
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

    // The CCOMPARE0 pending bit is level-triggered off the comparator: disabling the line
    // in INTENABLE stops re-fire until the kernel re-arms and clears the match.
    if ((pending & (1u << CCOMPARE0_INT)) != 0)
    {
        phys_int_disable(1u << CCOMPARE0_INT);
        kickos_isr_timer();
    }

    // Software doorbell (int 7): clear the latch first, or it redelivers forever. The
    // bound handler masks/acks as needed (tier-1 via irq_event_isr + irq_ack); do NOT mask
    // here, or a tier-2 irq_attach line stays masked forever after one delivery.
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

    // Real device lines (chip-bound): no doorbell and no g_irq_masked gating. The mask is
    // the CPU interrupt's INTENABLE bit, the clear is the driver's own peripheral register,
    // and a CPU interrupt several routes share is demuxed ONCE.
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
// LEVEL1INTERRUPT (cause 4) is demuxed to the ISR entry before this path, so it never
// reaches the reporter.
void kickos_lx6_fault_report(uint32_t exccause, uint32_t excvaddr,
                             uint32_t epc1, uint32_t ps)
{
    kpanic_enter();
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
uint64_t kickos_lx6_ccount_ns(void)
{
    return cycles_to_ns(now_cycles());
}

// --- Trace clock (telemetry timestamp seam) ---------------------------------
// Raw 32-bit CCOUNT; the host reconstructs absolute time from the SESSION clock_hz
// anchors. No wrap-extend and no crit section, so it is callable from the switch path.
uint32_t arch_trace_now(void)
{
    return rd_ccount();
}

uint32_t arch_cpu_clock_hz(void)
{
    return SystemCoreClock;
}


// Smallest CCOMPARE margin worth attempting; the arm loop below is what makes correctness
// independent of it.
static constexpr uint32_t CCOMPARE_MIN_CYCLES = 64;

void arch_timer_arm(uint64_t deadline_ns)
{
    uint64_t now = arch_clock_now();
    uint64_t delta_ns = 0;
    if (deadline_ns > now)
    {
        delta_ns = deadline_ns - now;
    }
    // Clamp to the 32-bit CCOMPARE range BEFORE converting, or a far-future deadline
    // overflows the ns*freq product. A clamped deadline fires early and the kernel re-arms
    // the remainder.
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
    // CCOMPARE0 is an EQUALITY match against a free-running CCOUNT, not a countdown: a
    // compare value already BEHIND CCOUNT when it lands is missed, not late, and the next
    // match is a full 2^32-cycle wrap away (about 18 s at 240 MHz). The few instructions
    // between reading CCOUNT and writing CCOMPARE can lose a one-cycle margin. The signed
    // difference is what makes the re-check wrap-correct.
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
    // The pending CCOMPARE0 match is cleared by the next wsr.ccompare0 (arch_timer_arm).
    phys_int_disable(1u << CCOMPARE0_INT);
}

// --- MPU: no hardware per-task protection on the classic ESP32 ---------------
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n,
                    struct arch_mpu_encoded const* image)
{
    (void)regions;
    (void)n;
    (void)image;
}

// LX6 has neither an MPU nor a ring split; the self-grant path still calls this.
void kickos_arch_mpu_commit(void) {}

// 0 keeps arch_ram_alloc byte-granular: there is nothing to enforce a region against.
size_t arch_mpu_min_region(void)
{
    return 0u;
}

// No descriptor to satisfy, so any nonzero 16-byte-aligned window is "encodable"; nothing
// is actually enforced.
bool arch_mpu_region_encodable(uintptr_t base, size_t size)
{
    if (size == 0)
    {
        return false;
    }
    return (base & 15u) == 0 and (size & 15u) == 0;
}

// arch_mpu_min_region() is 0 here, which short-circuits both inlines before the mode is
// reached.
int arch_mpu_region_pow2(void)
{
    return 1;
}

// Internal SRAM, where the arena lives, is not cached on the classic ESP32: the cache
// covers external flash and PSRAM only.
int arch_mpu_nocache_support(void)
{
    return ARCH_MPU_NOCACHE_ALREADY;
}



// --- The kernel-owned mask for a REAL device line (RULE L1) -------------------
// Clears / sets the INTENABLE bit of the CPU interrupt the matrix drives the device to.
// INTENABLE is core state reached only by rsr/wsr, so it can never be delegated to a
// driver; the driver's own UART_INT_ENA is off limits to the kernel.
// A line with no device route is an injected software line: no-op.
// The INTENABLE read-modify-write is atomic only because phys_int_* raises PS.INTLEVEL
// around it and closes with rsync; a bare wsr.intenable would be neither.
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
// logical line cannot be a physical INTENABLE bit: lines 5/9/11 would be silent no-ops and
// line 6 collides with the timer. Mask is a software bitmask; inject records the line and
// rings the ONE real software int 7 (dispatched in kickos_lx6_dispatch_l1).
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
    // A raise taken while this line was masked redelivers now through the int-7 doorbell,
    // on the normal ISR path.
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
    // Bracketed like arch_irq_mask/unmask: an ISR reaching those writes the same words.
    arch_irq_state_t s = arch_irq_save();
    // A raise on a masked line latches one-deep (redelivered at unmask), it is NOT
    // dropped.
    if ((g_irq_masked & (1u << (static_cast<unsigned>(irq) & 31u))) != 0)
    {
        g_irq_pending = g_irq_pending | (1u << (static_cast<unsigned>(irq) & 31u));
    }
    else
    {
        // recorded BEFORE ringing the doorbell (the dispatcher reads it)
        g_inject_line = irq;
        uint32_t bit = 1u << SW_INT_L1;
        // Enabled JUST-IN-TIME, disabled again by the dispatcher: the ROM boots with int 7
        // pending, so a doorbell left enabled at rest storms the level-1 handler.
        phys_int_enable(bit);
        __asm volatile("wsr.intset %0; rsync" ::"a"(bit) : "memory");
    }
    arch_irq_restore(s);
}

// --- Device-route bind (chip layer) -----------------------------------------
// Adds one (CPU interrupt, logical line) route and arms that CPU interrupt in INTENABLE.
// Several lines may name the same cpu_int (the grouped-line shape).
// Call ONLY from arch_init, before any interrupt is enabled: the route arrays are read in
// ISR context and written here with no critical section. A route past LX6_DEV_ROUTES is
// dropped silently.
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
    __asm volatile("waiti 0"); // wait for interrupt at level 0
}

// --- Syscall: a plain call, since there is no CPU ring split and so no trap ---
// A blocking syscall blocks by an ordinary synchronous arch_switch.
//
// So this backend ships no ipc_fastpath.cmake, and that is a property of the silicon
// rather than a port left undone. The trap-handler IPC fastpath exists to skip an
// exception entry, a privileged-thread trampoline and a deferred switch back; none of the
// three happens below, where the dispatch is a call the caller makes itself. There is also
// no saved register frame for the reply to land in and no return address to redirect, so
// Thread::call_frame_parked cannot be given a meaning here. A fastpath on this arch would
// be the generic path wearing another name.
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
    // Every physical line masked: the timer enables CCOMPARE0 on arm and arch_irq_inject
    // enables the int-7 doorbell just-in-time. Logical device lines are gated by
    // g_irq_masked, not INTENABLE.
    wr_intenable(0);
    // Coprocessor 0 (the single-precision FPU), on for every thread. CPENABLE is global,
    // not per-thread: the switch banks the FP data registers, not this enable. FP regs are
    // caller-saved, so only the preemptive path (the level-1 interrupt frame, startup.S)
    // saves f0-f15+FCR+FSR; the cooperative switch relies on the compiler's spill.
    __asm volatile("wsr.cpenable %0; rsync" ::"a"(1u) : "memory"); // CP0 bit0
    g_cyc_high = 0;
    g_cyc_last = 0;
}

}
