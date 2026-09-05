// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
#include <kickos/arch/arch.h>
#include <kickos/arch/lx6_doorbell.h>
#include <kickos/arch/lx6_trap_stack.h> // the figures check_trap_redzone.sh enforces
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

// 0 keeps only the one-line fault marker; set it in the board defconfig or with
// cmake -DKICKOS_PANIC_DUMP=0.
#ifndef KICKOS_PANIC_DUMP
#define KICKOS_PANIC_DUMP 1
#endif

// switch.S + startup.S hard-code these arch_context field offsets.
static_assert(F_SIZE == KICKOS_LX6_TRAP_FRAME,
              "_kickos_int_level1 subtracts F_SIZE; the gate enforces KICKOS_LX6_TRAP_FRAME");

// Idle never enters a syscall, so KICKOS_MIN_STACK_SIZE does not bind it; what does is the
// involuntary frame plus the dispatch below it, which is this whole zone.
static_assert(KICKOS_IDLE_STACK_SIZE >= KICKOS_LX6_TRAP_FRAME + KICKOS_LX6_TRAP_DEPTH,
              "the level-1 interrupt zone does not fit this board's idle stack, so idle dies on "
              "its first preemption");

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
    // can latch. The flag is software-set only, so the dispatcher must wsr.intclear it or
    // one inject redelivers forever.
    constexpr int SW_INT_L1 = 7;

    // XCHAL_INTLEVEL1_MASK for the LX6 config: only these lines reach the level-1 entry;
    // higher-level lines have their own vectors.
    constexpr uint32_t KICKOS_L1_INT_MASK = 0x000637FFu;

    // Wrap extension of the 32-bit CCOUNT cycle counter. LIMITATION: at 240 MHz CCOUNT
    // wraps every ~17.9 s, and a wrap not observed within one 2^32-cycle period is missed.
    // The two words are ONE value, kept coherent by the IrqLock in now_cycles.
    // Per core: each CPU counts its own cycles and the two are not synchronised. No caller on
    // this chip, where chip_esp32.cc overrides arch_clock_now with the TIMG count.
    uint32_t g_cyc_high[KICKOS_NUM_CORES] = {};
    uint32_t g_cyc_last[KICKOS_NUM_CORES] = {};

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

    // PHYSICAL enable/disable (INTENABLE). The public arch_irq_* seam is the software
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

    // ATOMCTL (SR 99) selects PER MEMORY CLASS whether S32C1I issues an RCW bus transaction,
    // operates inside this core, or raises LoadStoreErrorCause (ISA summary 4.3.13.4, Table 52
    // p.121). The core-local arm excludes nothing across CPUs and faults nowhere. The
    // architectural reset value 0x28 (Table 190, p.313) selects it for both cacheable classes.
    // 0x15 is WB=WT=BY=1, the all-RCW value.
    constexpr uint32_t ATOMCTL_ALL_RCW = 0x15u;
    // Bits above these are not S32C1I's: bit 8 is the Exclusive Access Option's flag and
    // 7:6 are undefined to write.
    constexpr uint32_t ATOMCTL_S32C1I_FIELDS = 0x3Fu;

    char const ATOMCTL_REFUSED[] =
        "KickOS: lx6 ATOMCTL does not select the RCW bus transaction, read 0x";
    char const ATOMCTL_NL[] = "\n";

    void hex2(uint32_t v)
    {
        for (int shift = 4; shift >= 0; shift -= 4)
        {
            uint32_t const nib = (v >> shift) & 0xFu;
            char c = static_cast<char>('0' + nib);
            if (nib > 9u)
            {
                c = static_cast<char>('a' + nib - 10u);
            }
            arch_console_write_sync(&c, 1);
        }
    }

    // Every core runs this before it can reach a kernel lock. The read-back matters: a part
    // whose ATOMCTL fields are not writable would leave S32C1I excluding nothing, silently.
    void atomctl_seat_or_refuse(void)
    {
        __asm volatile("wsr.atomctl %0; rsync" ::"a"(ATOMCTL_ALL_RCW) : "memory");
        uint32_t got = 0;
        __asm volatile("rsr.atomctl %0" : "=a"(got));
        if ((got & ATOMCTL_S32C1I_FIELDS) != ATOMCTL_ALL_RCW)
        {
            arch_console_write_sync(ATOMCTL_REFUSED, sizeof(ATOMCTL_REFUSED) - 1);
            hex2(got);
            arch_console_write_sync(ATOMCTL_NL, sizeof(ATOMCTL_NL) - 1);
            kfault_terminate();
        }
    }

#if KICKOS_NUM_CORES > 1
    char const UNSEATED[] =
        "KickOS: lx6 core took a level-1 interrupt before its own init seated it\n";

#if KICKOS_DEBUG
    char const WRONG_CORE[] =
        "KickOS: lx6 device line touched from a core it is not routed to\n";
#endif

    // Nonzero once THIS core has reached its own landing and seated itself; written by that
    // core alone, read by the primary's release. The acquire/release pair is load-bearing: the
    // primary must see every register and route the far core seated BEFORE it sees the arrival.
    kickos::Atomic<uint32_t, kickos::Order::ACQUIRE | kickos::Order::RELEASE>
        g_core_arrived[KICKOS_NUM_CORES] = {};

    // Nonzero once kickos_lx6_init has run ON THIS CORE; read by the level-1 dispatch to
    // refuse an interrupt taken before the state it needs exists.
    uint32_t g_core_seated[KICKOS_NUM_CORES] = {};

    // The chip's doorbell input, cached by kickos_lx6_init. Written before this core's
    // interrupts open and read-only afterwards, so it carries no ordering.
    uint32_t g_doorbell_cpu_int = 0;
#endif

    // Real device lines: the matrix routes a peripheral source to a CPU interrupt, and the
    // chip demuxes that CPU interrupt into logical lines. INTENABLE is their kernel-owned
    // mask (RULE L1); the peripheral's own enable register belongs to the driver.
    // The mask is COARSE: several lines may name the same cpu_int, and masking one of them
    // masks its siblings.
    constexpr unsigned LX6_DEV_ROUTES = 4;
    Atomic<int8_t, Order::RELAXED> g_dev_cpu_int[LX6_DEV_ROUTES] = {-1, -1, -1, -1};
    Atomic<int8_t, Order::RELAXED> g_dev_line[LX6_DEV_ROUTES] = {-1, -1, -1, -1};
    // Which core takes the line: the pin freeze N3 rests on, and INTENABLE is per core, so
    // this says whose bit gets armed. Stored above one core only, though
    // kickos_lx6_bind_dev_int's core argument is unconditional.
#if KICKOS_NUM_CORES > 1
    Atomic<int8_t, Order::RELAXED> g_dev_core[LX6_DEV_ROUTES] = {-1, -1, -1, -1};
#endif

    // The CPU interrupt a logical line's mask targets, or -1 for a line with no route
    // (an injected software line, whose mask is its g_irq_unmasked cell alone).
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

    // The core a logical line is routed to, or -1 for a line with no device route.
    inline int dev_route_core(int line)
    {
#if KICKOS_NUM_CORES > 1
        int8_t const want = static_cast<int8_t>(static_cast<unsigned>(line) & 31u);
        for (unsigned i = 0; i < LX6_DEV_ROUTES; i++)
        {
            if (g_dev_line[i] == want)
            {
                return g_dev_core[i];
            }
        }
#else
        (void)line;
#endif
        return -1;
    }

    // Refuses a touch of a ROUTED line from a core it is not routed to (freeze N3). An
    // UNROUTED line has no routed core and is not covered here; what carries those cells is
    // stated at the declaration of g_irq_unmasked.
    inline void assert_line_core(int line)
    {
#if KICKOS_DEBUG && KICKOS_NUM_CORES > 1
        int const core = dev_route_core(line);
        if (core >= 0 and core != static_cast<int>(arch_cpu_id()))
        {
            arch_console_write_sync(WRONG_CORE, sizeof(WRONG_CORE) - 1);
            kfault_terminate();
        }
#else
        (void)line;
#endif
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
    // (0..N logical lines), and owns the per-source clear discipline. A chip that binds a
    // device route defines this or fails to LINK.
    void kickos_lx6_dispatch_dev(int cpu_int);

    // Shared with switch.S/arch_start/startup.S, written by C and by asm: the ctx of the
    // running thread, and the deferred-switch target when arch_switch runs in ISR context.
    // Per core; the asm indexes them through PERCPU_CELL (lx6_percpu.h).
    kickos::Atomic<struct arch_context*, kickos::Order::RELAXED>
        g_arch_current[KICKOS_NUM_CORES] = {};
    kickos::Atomic<struct arch_context*, kickos::Order::RELAXED>
        g_arch_next[KICKOS_NUM_CORES] = {};

    // switch.S and the chip startup.S load each as a plain word, and index it with addx4.
    static_assert(sizeof(g_arch_current[0]) == sizeof(struct arch_context*), "asm reads one word");
    static_assert(sizeof(g_arch_next[0]) == sizeof(struct arch_context*), "asm reads one word");
    static_assert(sizeof(g_arch_current[0]) == 4, "PERCPU_CELL indexes with addx4");
    static_assert(alignof(decltype(g_arch_current[0])) == alignof(struct arch_context*), "asm reads it naturally aligned");

    // Set by arch_switch when it defers a switch from ISR context; consumed and cleared by
    // the level-1 interrupt exit (startup.S _kickos_int_level1). A flag of its own, because
    // the cooperative path advances g_arch_current without touching g_arch_next, so
    // g_arch_current != g_arch_next does not mean a preemption is pending.
    kickos::Atomic<uint32_t, kickos::Order::RELAXED> g_arch_switch_pending[KICKOS_NUM_CORES] = {};

    // In-ISR depth (the IPSR!=0 analog), maintained by the level-1 interrupt entry
    // (startup.S).
    uint32_t g_isr_depth[KICKOS_NUM_CORES] = {};

    // startup.S carries both with l32i/s32i, and indexes them with addx4.
    static_assert(sizeof(g_arch_switch_pending[0]) == 4, "asm reads one word");
    static_assert(sizeof(g_isr_depth[0]) == 4, "asm reads one word");

    // Software interrupt controller for the LOGICAL device lines (arch.h inject/mask
    // contract). Xtensa INTSET only latches the software-type lines (int 7/29), so a
    // logical line cannot be a physical bit: a raise records the line here and rings the
    // ONE real software doorbell (int 7). The timer (CCOMPARE0) and that doorbell are the
    // only PHYSICAL lines.
    // 0 = masked. All lines start MASKED at reset (the arch.h reset contract), which
    // zero-initialisation gives, so the array lands in .bss.
    //
    // Image-wide and not per core (freeze N3): a ROUTED line is pinned to one core, so the
    // RSIL bracket below is its whole exclusion. An UNROUTED line has no pin, and
    // arch_irq_mask reached from irq_event_isr is then the one writer in the image holding no
    // kernel lock. That is what the cell must be atomic against; arch_irq_save masks only THIS
    // core's levels and orders nothing across cores.
    //
    // Nothing orders that ISR mask against a waiter's unmask except the kernel lock's own
    // chain: irq_event_isr masks, then sem_post takes IrqLock, and S32C1I plays both acquire
    // and release (ISA summary 4.3.13.5, p.122). Dropping that lock from sem_post, or moving
    // the mask after it, breaks this cell and reddens no arm. Order::RELAXED because a release
    // here would order the wrong side.
    //
    // 32 cells because every index below is `& 31u`; KICKOS_MAX_IRQ is kernel-layer and this
    // layer carries no kernel include path (arch/CMakeLists.txt).
    static kickos::Atomic<uint8_t, kickos::Order::RELAXED> g_irq_unmasked[32] = {};
    // pending software-injected logical line
    // Per core: arch_irq_inject raises INTSET on the CALLING core, so that same core's
    // dispatch must read it. The -1 sentinel is spelled per element because 0 is a valid line.
    static_assert(KICKOS_NUM_CORES <= 2, "the sentinel below lists one value per core");
#if KICKOS_NUM_CORES > 1
    static kickos::Atomic<int, kickos::Order::RELAXED> g_inject_line[KICKOS_NUM_CORES] = {-1, -1};
#else
    static kickos::Atomic<int, kickos::Order::RELAXED> g_inject_line[KICKOS_NUM_CORES] = {-1};
#endif
    // set = a raise landed on this logical line while masked (latched one-deep, coalesced).
    // Redelivered through the int-7 doorbell at unmask.
    //
    // Serialised by the kernel lock at every access, unlike g_irq_unmasked: no ISR-context
    // path reaches it, both callers of irq_line_op_local asking only for MASK.
    static kickos::Atomic<uint8_t, kickos::Order::RELAXED> g_irq_pending[32] = {};

}

namespace
{
    using namespace kickos::units; // _s == 1e9 ns

    inline uint64_t now_cycles()
    {
        // The wrap-extend read must be atomic against a concurrent reader (thread + ISR).
        arch_irq_state_t s = arch_irq_save();
        uint32_t cur = rd_ccount();
        uint32_t const core = arch_cpu_id();
        if (cur < g_cyc_last[core])
        {
            ++g_cyc_high[core];
        }
        g_cyc_last[core] = cur;
        uint64_t hi = g_cyc_high[core];
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
// (_kickos_lx6_irq_restore, startup.S). A fabricated `retw` underflow start would leave the
// trampoline a phantom windowed frame whose base-save-area caller linkage is invalid, and a
// non-blocking entry->run->exit wraps WindowBase around the whole 64-AR file, overflowing
// that frame through the invalid linkage into data RAM.
//
// `entry` rotates the window by PS.CALLINC and, for a call4 frame, maps the caller's a6/a7
// to the callee's a2/a3 (Xtensa ISA, Windowed Register Option).
void arch_context_init(struct arch_context* ctx,
                       void (*entry)(void*), void* arg,
                       void* stack_base, size_t stack_size,
                       int privileged)
{
    (void)privileged; // LX6 runs every thread at one privilege level

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
    uint32_t const core = arch_cpu_id();
    if (g_isr_depth[core] != 0)
    {
        // The physical swap happens at level-1 interrupt exit (startup.S
        // _kickos_int_level1), which saves g_arch_current in the interrupt-frame format.
        g_arch_next[core] = to;
        g_arch_switch_pending[core].store(1);
        return;
    }
    g_arch_current[core] = to;
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
    return g_isr_depth[arch_cpu_id()] != 0;
}

// --- Level-1 interrupt dispatch --------------------------------------------
// Runs at INTLEVEL=1 in thread-style windowed context on the interruptee stack.
// g_isr_depth is bumped by the asm entry, so arch_in_isr() reads true here.
void kickos_lx6_dispatch_l1(void)
{
#if KICKOS_NUM_CORES > 1
    // PERCPU_CELL indexes on one bit of PRID and refuses no third value; what makes that safe
    // is kickos_lx6_init having run on this core before its interrupts opened.
    if (g_core_seated[arch_cpu_id()] == 0u)
    {
        arch_console_write_sync(UNSEATED, sizeof(UNSEATED) - 1);
        kfault_terminate();
    }
#endif
    uint32_t pending = rd_interrupt() & rd_intenable() & KICKOS_L1_INT_MASK;

    // The CCOMPARE0 pending bit is level-triggered off the comparator: disabling the line
    // in INTENABLE stops re-fire until the kernel re-arms and clears the match.
    if ((pending & (1u << CCOMPARE0_INT)) != 0)
    {
        phys_int_disable(1u << CCOMPARE0_INT);
        kickos_isr_timer();
    }

#if KICKOS_NUM_CORES > 1
    // The doorbell's own matrix-routed LEVEL input: pending follows the trigger register, so
    // the chip's clear is the whole acknowledgement. No INTCLEAR, and INTENABLE stays open.
    if ((pending & (1u << g_doorbell_cpu_int)) != 0)
    {
        // Before any service: a set landing after the clear re-asserts the input.
        kickos_lx6_doorbell_clear();
        // Takes no kernel lock: an initiator may be holding it while it waits here. The cell
        // is the authority, so a spurious entry finds nothing owed and returns.
        if (kickos_lx6_doorbell_pending() != 0)
        {
            kickos_lx6_doorbell_service();
        }
#if KICKOS_KERNEL_CORES > 1
        // Outside the service body, because it takes the kernel lock.
        if (kickos_kernel_core_resched_take() != 0)
        {
            kickos_kernel_core_resched();
        }
#endif
    }
#endif

    // Software doorbell (int 7): clear the latch first, or it redelivers forever. The
    // bound handler masks/acks as needed (tier-1 via irq_event_isr + irq_ack); do NOT mask
    // here, or a tier-2 irq_attach line stays masked forever after one delivery.
    if ((pending & (1u << SW_INT_L1)) != 0)
    {
        uint32_t bit = 1u << SW_INT_L1;
        __asm volatile("wsr.intclear %0; rsync" ::"a"(bit) : "memory");
        phys_int_disable(bit); // doorbell consumed: off until the next inject re-arms it
        int line = g_inject_line[arch_cpu_id()];
        g_inject_line[arch_cpu_id()] = -1;
        if (line >= 0)
        {
            kickos_isr_irq(line);
        }
    }

    // Real device lines (chip-bound). The mask is the CPU interrupt's INTENABLE bit, the
    // clear is the driver's own peripheral register, and a CPU interrupt several routes
    // share is demuxed ONCE.
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

// --- MPU: LX6 answers every seam so the shared kernel paths run unchanged -----
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n,
                    struct arch_mpu_encoded const* image)
{
    (void)regions;
    (void)n;
    (void)image;
}

// Nothing to program on this backend.
void kickos_arch_mpu_commit(void) {}

// 0 keeps arch_ram_alloc byte-granular.
size_t arch_mpu_min_region(void)
{
    return 0u;
}

// Any nonzero 16-byte-aligned window is encodable, and the answer is advisory: it feeds
// the admission arithmetic, never a register.
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

// The classic ESP32 cache covers external flash and PSRAM only, and the arena lives in
// internal SRAM.
int arch_mpu_nocache_support(void)
{
    return ARCH_MPU_NOCACHE_ALREADY;
}



// --- The kernel-owned mask for a REAL device line (RULE L1) -------------------
// Clears / sets the INTENABLE bit of the CPU interrupt the matrix drives the device to.
// INTENABLE is core state reached only by rsr/wsr, so the kernel owns it outright and the
// driver's own UART_INT_ENA stays the driver's.
// An injected software line has no device route, and returns leaving INTENABLE alone.
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
    assert_line_core(line);
    arch_irq_state_t s = arch_irq_save();
    g_irq_unmasked[static_cast<unsigned>(line) & 31u] = 0u;
    kickos_lx6_hw_mask(line);
    arch_irq_restore(s);
}

void arch_irq_unmask(int line)
{
    if (line < 0)
    {
        return;
    }
    assert_line_core(line);
    unsigned l = static_cast<unsigned>(line) & 31u;
    arch_irq_state_t s = arch_irq_save();
    g_irq_unmasked[l] = 1u;
    kickos_lx6_hw_unmask(line);
    // A raise taken while this line was masked redelivers now through the int-7 doorbell,
    // on the normal ISR path.
    if (g_irq_pending[l] != 0u)
    {
        g_irq_pending[l] = 0u;
        g_inject_line[arch_cpu_id()] = static_cast<int>(l);
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
    assert_line_core(line);
    arch_irq_state_t s = arch_irq_save();
    g_irq_pending[static_cast<unsigned>(line) & 31u] = 0u;
    arch_irq_restore(s);
}

// The matrix routes a source into exactly one CPU's bank and sinks it in the other
// (kickos_lx6_bind_dev_int), so a bound line HAS a core and an unbound one does not.
int arch_irq_line_core(int line)
{
    if (line < 0)
    {
        return KICKOS_IRQ_LINE_CORE_NONE;
    }
    return dev_route_core(line);
}

void arch_irq_inject(int irq)
{
    if (irq < 0)
    {
        return;
    }
    assert_line_core(irq);
    // Bracketed like arch_irq_mask/unmask: an ISR reaching those writes the same cells.
    arch_irq_state_t s = arch_irq_save();
    // A raise on a masked line latches one-deep (redelivered at unmask), it is NOT
    // dropped.
    if (g_irq_unmasked[static_cast<unsigned>(irq) & 31u] == 0u)
    {
        g_irq_pending[static_cast<unsigned>(irq) & 31u] = 1u;
    }
    else
    {
        // recorded BEFORE ringing the doorbell (the dispatcher reads it)
        g_inject_line[arch_cpu_id()] = irq;
        uint32_t bit = 1u << SW_INT_L1;
        // Enabled JUST-IN-TIME, disabled again by the dispatcher: the ROM boots with int 7
        // pending, so a doorbell left enabled at rest storms the level-1 handler.
        phys_int_enable(bit);
        __asm volatile("wsr.intset %0; rsync" ::"a"(bit) : "memory");
    }
    arch_irq_restore(s);
}

// --- Device-route bind (chip layer) -----------------------------------------
// Adds one (CPU interrupt, logical line, core) route and arms that CPU interrupt in INTENABLE
// on the core that takes it. Several lines may name the same cpu_int (the grouped-line shape).
//
// The CHIP owes the other half of the pin: pointing the source at that core's bank and sinking
// it in the other.
//
// INTENABLE IS PER CORE, so a route naming a core that is not this one is RECORDED and not
// armed here: that core arms it from its own kickos_lx6_init. Call ONLY from arch_init, before
// any interrupt is enabled: the route arrays are read in ISR context and written here with no
// critical section. A route past LX6_DEV_ROUTES is dropped silently.
void kickos_lx6_bind_dev_int(int cpu_int, int line, int core)
{
    if (cpu_int < 0 or cpu_int > 31 or line < 0)
    {
        return;
    }
    if (core < 0 or core >= static_cast<int>(KICKOS_NUM_CORES))
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
#if KICKOS_NUM_CORES > 1
        g_dev_core[i] = static_cast<int8_t>(core);
#endif
        g_dev_cpu_int[i] = static_cast<int8_t>(cpu_int);
        if (core == static_cast<int>(arch_cpu_id()))
        {
            phys_int_enable(1u << static_cast<unsigned>(cpu_int));
        }
        return;
    }
}

// --- Idle -------------------------------------------------------------------
void arch_idle_wait(void)
{
    __asm volatile("waiti 0"); // wait for interrupt at level 0
}

// --- Syscall: a plain call on a core with one privilege level -----------------
// A blocking syscall blocks by an ordinary synchronous arch_switch.
//
// The trap-handler IPC fastpath earns its keep by skipping an exception entry, a
// privileged-thread trampoline and a deferred switch back. Below, the dispatch is a call
// the caller makes itself, which already skips all three, so a fastpath here would be the
// generic path wearing another name: the silicon settles it, and this backend ships no
// ipc_fastpath.cmake. Thread::call_frame_parked needs a saved register frame to seat a
// reply in, and this path answers in the caller's own registers.
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

// --- One-time core bring-up ------------------------------------------------
#if KICKOS_NUM_CORES > 1
// Published by the arriving core itself; read by the primary's bounded release.
void kickos_lx6_core_arrived_set(void)
{
    g_core_arrived[arch_cpu_id()].store(1u);
}

uint32_t kickos_lx6_core_arrived(uint32_t core)
{
    if (core >= KICKOS_NUM_CORES)
    {
        return 0u;
    }
    return g_core_arrived[core].load();
}
#endif

// Every core runs this ON ITSELF: every register it writes is per core. It must run before
// this core's interrupts open, which is what the assembly core index rests on (chip_cpuid.h).
void kickos_lx6_init(void)
{
    // Before this core can take the kernel lock: arch_kernel_lock claims with S32C1I.
    atomctl_seat_or_refuse();
    // Every physical line masked: the timer enables CCOMPARE0 on arm and arch_irq_inject
    // enables the int-7 software line just-in-time. Logical device lines are gated by
    // g_irq_unmasked, not INTENABLE.
    wr_intenable(0);
#if KICKOS_NUM_CORES > 1
    // Seated before the route, which is what makes the input live.
    g_doorbell_cpu_int = kickos_lx6_doorbell_cpu_int();
    kickos_lx6_doorbell_route();
    phys_int_enable(1u << g_doorbell_cpu_int);
#endif
    // Coprocessor 0 (the single-precision FPU), on for every thread. CPENABLE is per CORE and
    // not per thread: the switch banks the FP data registers, not this enable. FP regs are
    // caller-saved, so only the preemptive path (the level-1 interrupt frame, startup.S)
    // saves f0-f15+FCR+FSR; the cooperative switch relies on the compiler's spill.
    __asm volatile("wsr.cpenable %0; rsync" ::"a"(1u) : "memory"); // CP0 bit0
    g_cyc_high[arch_cpu_id()] = 0;
    g_cyc_last[arch_cpu_id()] = 0;
#if KICKOS_NUM_CORES > 1
    // LAST, once every register and route above is in place.
    g_core_seated[arch_cpu_id()] = 1u;
    // Arm the routes bound to THIS core: a secondary runs after the primary has bound them all.
    for (unsigned i = 0; i < LX6_DEV_ROUTES; i++)
    {
        int const ci = g_dev_cpu_int[i];
        if (ci >= 0 and g_dev_core[i] == static_cast<int8_t>(arch_cpu_id()))
        {
            phys_int_enable(1u << static_cast<unsigned>(ci));
        }
    }
#endif
}

#if KICKOS_NUM_CORES > 1
// Where a released secondary lands, from _kickos_lx6_core1_entry. Seats everything about this
// core that the primary could not seat from over there, then hands it to the park.
void kickos_lx6_secondary_entry(void)
{
    kickos_lx6_init();
    // After the seating and before the park's unmask: the primary's release waits on this, and
    // what it must mean is that this core can take a doorbell.
    kickos_lx6_core_arrived_set();
    kickos_lx6_doorbell_park();
}
#endif

}
