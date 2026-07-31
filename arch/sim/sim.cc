// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/arch/arch.h>
#include <kickos/arch/clk_q32.h> // KICKOS_NS_PER_SEC (canonical 1e9 ns/sec)
#include <kickos/console_tx.h>

#include <ucontext.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>

#include <kickos/sys/errno.h> // arch_periph_reg_write's refusal taxonomy

// Pre-4.17 headers: the returned-address check at the mmap call site is the guard.
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0
#endif

#include <kickos/trace/record.h> // ArchId: pin this build's trace-arch id to this backend
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
#include <kickos/rtt.h>
#endif

static_assert(KICKOS_TRACE_ARCH == kickos::trace::ARCH_SIM,
              "KICKOS_TRACE_ARCH does not match ArchId::ARCH_SIM for sim");

// Kernel entry points used by the sim's fault reporters (sim.cc does not pull in
// kernel.h; kfault_terminate is defined below, in the extern "C" block).
namespace kickos
{
    void kprintf(char const* fmt, ...) __attribute__((format(printf, 1, 2)));
}
extern "C" void kpanic_enter(void);
extern "C" void kfault_terminate(void);

namespace
{

    // --- Internal context layout over the opaque arch_context storage ----------
    struct SimContext
    {
        ucontext_t uc;
        void (*entry)(void*);
        void* arg;
        volatile int raised; // >0 while mid-syscall (read from signal-driven switch)
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
        uint16_t tid; // owning thread's trace id (arch_trace_stamp_id)
#endif
    };
    static_assert(sizeof(SimContext) <= ARCH_CONTEXT_SIZE,
                  "SimContext exceeds ARCH_CONTEXT_SIZE; grow arch/sim context.h");

    inline SimContext* sc(struct arch_context* c)
    {
        return reinterpret_cast<SimContext*>(c->opaque);
    }

    // getcontext() is returns-twice, so values live across it in the function that calls
    // it are -Wclobbered: fatal above -O0, and it fires or not depending on instrumentation
    // (plain -Os is clean, -Os plus UBSan is not). noinline is load-bearing -- it keeps the
    // call out of arch_context_init, whose stack parameters are live across it.
    __attribute__((noinline)) void context_capture(ucontext_t* uc)
    {
        getcontext(uc);
    }

    // Instance-scoped sim backend state (invariant #7, the arch half): several sim
    // backends (one per emulated MCU / KickCAT slave) co-reside in one host
    // process, mirroring the kernel() seam but staying arch-side (invariant #1).
    struct SimInstance
    {
        // --- running-context tracking + deferred-switch state ---
        SimContext* current = nullptr; // arch's view of the running ctx
        volatile sig_atomic_t isr_depth = 0;

        // --- signal set covering all "interrupt" sources (crit-section mask) ---
        sigset_t irq_signals;

        // --- tickless one-shot timer ---
        timer_t timer;
        bool timer_created = false;

        // --- MPU: page-granular user-RAM arena governed by the emulation ---
        long pagesize = 0;
        unsigned char* arena = nullptr; // the mmap'd user-RAM pool
        size_t arena_size = 0;
        size_t arena_used = 0;          // bump allocator (arch_ram_alloc)
        unsigned char* guard = nullptr; // a reserved arena page no domain owns
        // SIM_PVREG_SPAN bytes at SIM_PVREG_BASE, outside the arena. null => this host
        // refused the fixed mapping, so no DEV window is encodable and the
        // privileged-write seam declines.
        unsigned char* pvreg = nullptr;

        // The running thread's resting region set, remembered so the syscall raise
        // can be lowered back to exactly it. We keep the caller's pointer (its TCB
        // regions[], stable while it runs) rather than a fixed-size copy -- no cap,
        // no truncation regardless of KICKOS_MPU_MAX_REGIONS.
        struct arch_mpu_region const* applied = nullptr;
        size_t applied_n = 0;

        // --- emulated device IRQ hand-off (async-signal to ISR) ---
        volatile sig_atomic_t pending_irq = -1;
        // bit L set => line L masked (a raise latches, see irq_pending). All lines
        // start MASKED at reset (the
        // arch.h reset contract, matching the NVIC/RX silicon posture); a driver
        // unmasks its line (arch_irq_unmask, or irq_register) before use.
        volatile sig_atomic_t irq_masked = static_cast<sig_atomic_t>(0xFFFFFFFFu);
        // bit L set => a raise landed on line L while it was masked (latched one-
        // deep, coalesced). Redelivered through the ISR path at unmask -- mirrors
        // the NVIC holding ISPR pending independently of ISER.
        volatile sig_atomic_t irq_pending = 0;

        // --- emulated buffered-console TX-empty interrupt source ---
        // The console ring drains through a real SIGUSR1 delivery (isr_depth++ ->
        // kickos_isr_irq(TX line) -> console_tx_isr), exactly like a hardware TX
        // IRQ. tx_enabled is the peripheral-level enable (irq_enable/irq_disable);
        // tx_asserted is the level line. tx_budget is a SYNTHETIC per-ISR-delivery
        // slot budget (host stdout never blocks, so without it the ring would drain
        // in one shot and never fill/wrap) -- it constrains slot_free ONLY inside
        // the drain ISR, so the synchronous prime/flush/overflow paths still see an
        // always-free channel and never stall.
        volatile sig_atomic_t tx_enabled = 0;
        volatile sig_atomic_t tx_asserted = 0;
        volatile sig_atomic_t in_tx_isr = 0; // scopes the synthetic budget to the ISR
        int tx_budget = 0;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
        // Consume-once switch-emit hand-off. The physically-outgoing tid is armed
        // just before each ucontext swap; whichever context RESUMES (an existing
        // one right after its swapcontext, or a new one at the trampoline) consumes
        // it and emits the SWITCH record for {from -> current}.
        volatile sig_atomic_t switch_pending = 0;
        uint16_t switch_from = 0xFFFF;
#endif
    };

    // All-constant init keeps this in BSS; signal handlers read it, so the accessor
    // stays a bare global reference (async-signal-safe, zero-cost). The multi-slave
    // sim swaps in a per-host-thread instance where noted (Later).
    SimInstance g_sim;
    SimInstance& sim()
    {
#if defined(KICKOS_MULTI_INSTANCE)
        return *g_sim_tls;
#else
        return g_sim;
#endif
    }

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Arm the switch hand-off just before a ucontext swap: `from_tid` is the tid
    // of the context we are swapping AWAY from (the physically-outgoing side).
    void trace_switch_arm(uint16_t from_tid)
    {
        sim().switch_from = from_tid;
        sim().switch_pending = 1;
    }
    // Consume the hand-off on the RESUMING side and emit {from -> current}. Called
    // right after every swapcontext and at the trampoline (a new thread's first
    // run). No-op if nothing armed (defensive; every swap arms exactly one).
    void trace_emit_switch_in()
    {
        if (sim().switch_pending == 0)
        {
            return;
        }
        sim().switch_pending = 0;
        uint16_t from = sim().switch_from;
        uint16_t to = static_cast<uint16_t>(kickos::trace::TRACE_NO_THREAD);
        if (sim().current != nullptr)
        {
            to = sim().current->tid;
        }
        kickos_trace_switch_done(from, to);
    }
#endif

    // --- Fake write-PV-only register block (arch_periph_reg_write's sim backend) ---
    // A DEV grant needs a base the app names at spawn time, so it must be known to both
    // sides at compile time. ONE fixed address would make the premise a bet on this
    // process's address space, so the first candidate that maps is published as
    // sim().pvreg and is the only base the seam and the encoder answer for.
    // user/apps/common/selftest walks the SAME list in the SAME order. Keep them identical.
    constexpr uintptr_t SIM_PVREG_BASES[] = {
        0x40000000u,          // 1 GiB
        0x100000000ull,       // 4 GiB
        0x400000000ull,       // 16 GiB
        0x10000000000ull,     // 1 TiB
        0x100000000000ull,    // 16 TiB
    };
    // 64 KiB: every host page size in practice (4/16/64 KiB) divides it, so the
    // "mprotect can describe this exactly" precondition below is satisfied by
    // construction rather than by the host happening to use 4 KiB pages.
    constexpr size_t SIM_PVREG_WINDOW = 0x10000u;
    // Twice the window, so an allowlist entry can sit OUTSIDE the grantable window with
    // host pages still behind it: a store there must be refused by the kernel's
    // containment check, never faulted on.
    constexpr size_t SIM_PVREG_SPAN = 2u * SIM_PVREG_WINDOW;

    struct SimPrivWriteReg
    {
        uintptr_t offset; // from the published base; there is only ever one block
        uint32_t mask;    // the only bits this entry may set
    };

    // Withholds whole bytes at both ends of the word, so no off-mask value can be
    // confused with an in-mask one: widening the column shows up as a store that lands.
    constexpr uint32_t PVREG_MASKED_GRANT = 0x0000C3FFu;
    // Reachable only by a holder of a window wider than SIM_PVREG_WINDOW, which this
    // backend never admits.
    constexpr uint32_t PVREG_BEYOND_GRANT = 0x00000001u;

    constexpr SimPrivWriteReg SIM_PRIV_WRITE_REGS[] = {
        { 0x010u, PVREG_MASKED_GRANT },
        { SIM_PVREG_WINDOW, PVREG_BEYOND_GRANT },
    };

    // An entry outside the mapping would fault INSIDE the privileged store, which ends
    // the whole system (kfault_terminate), so the span must cover every entry's word.
    static_assert(SIM_PRIV_WRITE_REGS[0].offset + sizeof(uint32_t) <= SIM_PVREG_SPAN
                      and SIM_PRIV_WRITE_REGS[1].offset + sizeof(uint32_t) <= SIM_PVREG_SPAN,
                  "a sim allowlist entry lies outside the mapped fake register block");
    // The second entry must NOT be reachable from the only grantable window.
    static_assert(SIM_PRIV_WRITE_REGS[1].offset >= SIM_PVREG_WINDOW,
                  "the containment entry must sit beyond the grantable window");
    // A base the window does not divide would hand the app an unnaturally-aligned DEV
    // window, which no enforcing backend would admit.
    static_assert(SIM_PVREG_BASES[0] % SIM_PVREG_WINDOW == 0
                      and SIM_PVREG_BASES[1] % SIM_PVREG_WINDOW == 0
                      and SIM_PVREG_BASES[2] % SIM_PVREG_WINDOW == 0
                      and SIM_PVREG_BASES[3] % SIM_PVREG_WINDOW == 0
                      and SIM_PVREG_BASES[4] % SIM_PVREG_WINDOW == 0,
                  "every candidate base must be window-aligned");

    void arena_lower_to_applied(); // defined below; used by the trampoline

    // --- Trampoline pointer packing (makecontext takes ints) -------------------
    void trampoline(unsigned hi, unsigned lo)
    {
        uintptr_t p = (static_cast<uintptr_t>(hi) << 32) | static_cast<uintptr_t>(lo);
        SimContext* c = reinterpret_cast<SimContext*>(p);
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
        // A new thread's first run resumes HERE (not after a swapcontext), so emit
        // its switch-in record before running its body. The ucontext was created
        // with IRQ signals blocked (arch_context_init) precisely so no ISR can
        // preempt between the physical swap and this emit; unblock them only now.
        trace_emit_switch_in();
        sigprocmask(SIG_UNBLOCK, &sim().irq_signals, nullptr);
#endif
        // Enter user code under this thread's OWN resting MPU posture. arch_mpu_apply
        // at the starting switch left the arena raised (and recorded this thread's set);
        // lower to it now -- on this thread's own stack, which the gap-based lower keeps
        // mapped -- so the thread is enforced from its first instruction, not only after
        // its first syscall. Mask the emulated IRQ lines across the transition so no ISR
        // reprograms the arena mid-way.
        {
            arch_irq_state_t s = arch_irq_save();
            arena_lower_to_applied();
            arch_irq_restore(s);
        }
        c->entry(c->arg);
        kickos_thread_return(); // noreturn
    }

    int prot_from_attr(uint32_t attr)
    {
        int prot = PROT_NONE;
        if (attr & ARCH_MPU_R)
        {
            prot |= PROT_READ;
        }
        if (attr & ARCH_MPU_W)
        {
            prot |= PROT_WRITE;
        }
        if (attr & ARCH_MPU_X)
        {
            prot |= PROT_EXEC;
        }
        return prot;
    }

    // Privileged posture: whole arena accessible (the background-region analog).
    // Used while KERNEL code runs: on hardware the privileged switch/syscall path is
    // exempt via the background region, so kernel code always reaches any RAM. The
    // sim mirrors that -- crucially, an unprivileged thread's own stack is now an
    // arena block, so kernel code (syscall dispatch, the scheduler) that runs ON that
    // stack MUST keep it mapped; raising the whole arena guarantees it.
    void arena_raise_all()
    {
        if (sim().arena != nullptr)
        {
            mprotect(sim().arena, sim().arena_size, PROT_READ | PROT_WRITE);
        }
    }
    // Is [base,base+size) a page-aligned sub-range of the arena? (fail-closed: a
    // bad/hostile grant must never mprotect host memory or de-execute code.)
    bool arena_region_valid(uintptr_t base, size_t size)
    {
        uintptr_t const astart = reinterpret_cast<uintptr_t>(sim().arena);
        size_t const pg = static_cast<size_t>(sim().pagesize);
        if (size == 0 or size > sim().arena_size)
        {
            return false;
        }
        if (base < astart or base - astart > sim().arena_size - size)
        {
            return false;
        }
        return (base - astart) % pg == 0 and size % pg == 0;
    }
    // Lower the arena from the raised (whole-RW) posture to the currently-running
    // thread's resting region set, WITHOUT ever transiently unmapping a granted
    // region. This is load-bearing: the caller runs on its OWN arena-resident stack,
    // which is one of these regions -- the old "PROT_NONE all; then re-grant" would
    // unmap that stack for the window in between and fault on the very next push.
    // Instead, since the arena is already whole-RW, PROT_NONE only the GAPS between
    // granted regions (leaving the regions, hence the live stack, mapped throughout),
    // then set each region to its exact attr (an RW->RO change never unmaps). Regions
    // are validated and insertion-sorted by base so the gaps are computed correctly.
    void arena_lower_to_applied()
    {
        if (sim().arena == nullptr)
        {
            return;
        }
        uintptr_t const astart = reinterpret_cast<uintptr_t>(sim().arena);
        uintptr_t const aend = astart + sim().arena_size;

        struct SortedRegion
        {
            uintptr_t base;
            size_t size;
            uint32_t attr;
        };
        // Local cap generously above KICKOS_MPU_MAX_REGIONS (8) without pulling a
        // kernel config header across the arch seam; extra entries are clamped.
        constexpr size_t CAP = 32;
        SortedRegion sorted[CAP];
        size_t m = 0;
        for (size_t i = 0; i < sim().applied_n and m < CAP; i++)
        {
            uintptr_t const b = sim().applied[i].base;
            size_t const s = sim().applied[i].size;
            if (not arena_region_valid(b, s))
            {
                continue;
            }
            size_t j = m;
            while (j > 0 and sorted[j - 1].base > b)
            {
                sorted[j] = sorted[j - 1];
                j--;
            }
            sorted[j].base = b;
            sorted[j].size = s;
            sorted[j].attr = sim().applied[i].attr;
            m++;
        }
        // PROT_NONE each gap; the granted regions themselves are never touched here,
        // so the live stack stays mapped across the whole transition.
        uintptr_t cursor = astart;
        for (size_t i = 0; i < m; i++)
        {
            if (sorted[i].base > cursor)
            {
                mprotect(reinterpret_cast<void*>(cursor), sorted[i].base - cursor, PROT_NONE);
            }
            uintptr_t const rend = sorted[i].base + sorted[i].size;
            if (rend > cursor)
            {
                cursor = rend;
            }
        }
        if (cursor < aend)
        {
            mprotect(reinterpret_cast<void*>(cursor), aend - cursor, PROT_NONE);
        }
        // Now pin each region to its exact rights (RW stays RW; a future RO region
        // narrows without unmapping).
        for (size_t i = 0; i < m; i++)
        {
            mprotect(reinterpret_cast<void*>(sorted[i].base), sorted[i].size,
                     prot_from_attr(sorted[i].attr));
        }
    }

    // Restore MPU state for the now-running context after a switch-in. A context
    // that is mid-syscall (raised) keeps privileged (whole-arena) access even
    // across a blocking switch back into it; otherwise the resting posture that
    // the last arch_mpu_apply() programmed for it already stands.
    void guard_apply_current()
    {
        if (sim().arena != nullptr and sim().current != nullptr and sim().current->raised > 0)
        {
            arena_raise_all();
        }
    }

    // Run an ISR body with correct interrupt-depth bookkeeping and, on the way
    // out at depth 0, perform any deferred context switch requested during it.
    void isr_frame_enter()
    {
        sim().isr_depth++;
    }
    void isr_frame_leave(SimContext* interrupted)
    {
        sim().isr_depth--;
        if (sim().isr_depth == 0 and sim().current != interrupted)
        {
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
            // Deferred (PendSV-analogue) swap site: physically-outgoing is the
            // interrupted context. Multiple wakes in one ISR collapse to this ONE
            // physical swap, so exactly one SWITCH record is emitted per ISR.
            trace_switch_arm(interrupted->tid);
#endif
            swapcontext(&interrupted->uc, &sim().current->uc);
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
            trace_emit_switch_in(); // `interrupted` resumed here on a later swap-in
#endif
        }
    }

    // --- Buffered console TX backend (console_tx.h) ----------------------------
    enum
    {
        TX_LINE = 30,       // < KICKOS_MAX_IRQ / SIM_IRQ_LINES; not used by any test/bench
        // Deliberately small: the sim's only reason to buffer is to exercise the
        // ring paths, so the ring is sized so ordinary console traffic WRAPS it
        // (usable 127 > the largest single burst these gates emit, so bursts still
        // take the fast enqueue+prime path rather than always overflowing) while
        // cumulative output crosses the index-mask boundary many times.
        TX_RING_SIZE = 128,  // power of two (index masking); usable capacity 127
        TX_BUDGET = 8       // bytes drained per ISR delivery (synthetic slot budget)
    };
    char g_tx_ring[TX_RING_SIZE];

    int sim_tx_slot_free()
    {
        // Sync paths (prime/flush/overflow) must never stall: host stdout is
        // infinitely fast, so report free. The budget only bites in ISR context.
        if (sim().in_tx_isr == 0)
        {
            return 1;
        }
        if (sim().tx_budget > 0)
        {
            return 1;
        }
        return 0;
    }

    void sim_tx_push(uint8_t b)
    {
        if (sim().in_tx_isr != 0 and sim().tx_budget > 0)
        {
            sim().tx_budget--;
        }
        char c = static_cast<char>(b);
        while (true)
        {
            ssize_t w = write(1, &c, 1);
            if (w == 1)
            {
                break;
            }
            // A scheduling signal (no SA_RESTART) can interrupt the write; retry
            // rather than drop the byte. Any hard error: nowhere left to report.
            if (w < 0 and errno == EINTR)
            {
                continue;
            }
            break;
        }
    }

    void sim_tx_irq_enable()
    {
        // Enabling the TX-empty IRQ on a channel with queued bytes asserts the
        // line. Called from console_tx_write under IrqLock (SIGUSR1 blocked), so
        // the raise stays pending until the lock releases -> the drain then runs
        // in genuine ISR context, not inline in the producer.
        sim().tx_enabled = 1;
        sim().tx_asserted = 1;
        raise(SIGUSR1);
    }

    void sim_tx_irq_disable()
    {
        sim().tx_enabled = 0;
        sim().tx_asserted = 0;
    }

    console_tx_backend const g_sim_tx_backend = {
        sim_tx_slot_free, sim_tx_push, sim_tx_irq_enable, sim_tx_irq_disable};

    // Run one TX-drain ISR delivery: refill the synthetic budget, dispatch the TX
    // line through the real IRQ table (kickos_isr_irq -> console_tx_isr), then, if
    // the peripheral IRQ is still enabled afterwards (ring not yet empty -- budget
    // ran out), re-assert the level line so the next SIGUSR1 continues the drain.
    // console_tx_isr calls irq_disable when the ring empties, which clears
    // tx_enabled and ends the chain. Bounded: every delivery drains >= TX_BUDGET
    // bytes from a finite ring, so it always terminates.
    void console_tx_service()
    {
        sim().tx_budget = TX_BUDGET;
        sim().in_tx_isr = 1;
        kickos_isr_irq(TX_LINE);
        sim().in_tx_isr = 0;
        if (sim().tx_enabled != 0)
        {
            sim().tx_asserted = 1;
            raise(SIGUSR1); // pending (SIGUSR1 blocked in-handler); redelivered on return
        }
    }

    void on_sigalrm(int, siginfo_t*, void*)
    {
        SimContext* interrupted = sim().current;
        isr_frame_enter();
        kickos_isr_timer();
        isr_frame_leave(interrupted);
    }

    void on_sigusr1(int, siginfo_t*, void*)
    {
        SimContext* interrupted = sim().current;
        isr_frame_enter();
        // The emulated TX-empty line shares this signal (a shared interrupt vector).
        // Consume the assertion; console_tx_service re-asserts if bytes remain.
        if (sim().tx_asserted != 0)
        {
            sim().tx_asserted = 0;
            console_tx_service();
        }
        int irq = sim().pending_irq;
        sim().pending_irq = -1;
        if (irq >= 0)
        {
            kickos_isr_irq(irq);
        }
        isr_frame_leave(interrupted);
    }

    void on_sigsegv(int, siginfo_t* si, void*)
    {
        uintptr_t addr = reinterpret_cast<uintptr_t>(si->si_addr);
        // Establish ISR context (parity with the ARM fault path, where IPSR is
        // non-zero in the fault handler): kickos_isr_fault reports via kprintf, and
        // the console routing guard MUST see arch_in_isr() to take the synchronous
        // writer -- else the fault line would be enqueued into the buffered ring
        // and lost when this handler _exit()s without draining it.
        isr_frame_enter();
        // We cannot distinguish read vs write portably here; report as write since
        // the demo/wild-write path is a store. Reporting routes through the kernel.
        kickos_isr_fault(addr, 1);
        // kickos_isr_fault is expected to terminate or recover; if it returns we
        // cannot safely resume the faulting store, so halt.
        arch_shutdown(2);
    }

    // Illegal instruction (host: x86 `ud2` from __builtin_trap): the sim's CPU-fault
    // reporter, the analogue of the MCU arch reporters (kickos_armv7m_fault_report et
    // al.). kpanic_enter masks signals, forces the synchronous polled writer, and
    // flushes the ring -- so the dump survives the ARMED console ring instead of being
    // enqueued and lost. Then the shared dead-end exits 132. Drives the fault-dump gate.
    void on_sigill(int, siginfo_t* si, void*)
    {
        isr_frame_enter();
        kpanic_enter();
        ::kickos::kprintf("\n=== SIM FAULT (illegal instruction) at %p ===\n", si->si_addr);
        kfault_terminate(); // -> arch_shutdown(132)
    }

    // Ctrl+C / kill: halt the sim cleanly instead of dying by default action.
    // Only async-signal-safe calls here (write + _exit).
    void on_sigterm(int, siginfo_t*, void*)
    {
        static char const msg[] = "\n[KickOS] halted.\n";
        ssize_t n = write(1, msg, sizeof(msg) - 1);
        (void)n;
        _exit(0);
    }

}

// ===========================================================================
extern "C"
{

void arch_init(void)
{
    sim().pagesize = sysconf(_SC_PAGESIZE);

    // The user-RAM arena the MPU emulation governs. It now hosts the demand-allocated
    // default thread stacks (one KICKOS_USER_STACK_SIZE block per convenient spawn,
    // reclaimed via the free list) on top of domain-data allocs and the probe page --
    // so it must fit the whole thread pool at once. The sim runs the fixed default
    // provisioning (KICKOS_MAX_THREADS=16 * KICKOS_USER_STACK_SIZE=64 KiB == 1 MiB);
    // 2 MiB leaves generous headroom for those small allocs and natural-alignment
    // padding. (Arch-pure: no kernel config header here; the host has ample RAM and a
    // real chip sizes its arena from the linker script.)
    sim().arena_size = 2 * 1024 * 1024;
    sim().arena = static_cast<unsigned char*>(
        mmap(nullptr, sim().arena_size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (sim().arena == MAP_FAILED)
    {
        sim().arena = nullptr; // MAP_FAILED is (void*)-1
        sim().arena_size = 0;
    }
    sim().arena_used = 0;
    sim().applied_n = 0;
    // Reserve one page no domain is ever granted: the isolation-probe address.
    sim().guard = static_cast<unsigned char*>(arch_ram_alloc(sim().pagesize));

    // The fake write-PV-only register block, OUTSIDE the arena: it must not be
    // reachable as a RAM grant, and arena_lower_to_applied must skip it. The store the
    // seam performs runs privileged in the kernel's own frame, so the pages must exist
    // before any allowlist entry is reachable. MAP_FIXED_NOREPLACE never evicts an
    // existing mapping, so a candidate this process already uses is skipped rather than
    // stolen. Kept if already mapped: a second arch_init must not drop the block that
    // the published base and every live DEV grant still refer to.
    if (sim().pvreg == nullptr)
    {
        for (uintptr_t cand : SIM_PVREG_BASES)
        {
            void* pv = mmap(reinterpret_cast<void*>(cand), SIM_PVREG_SPAN,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (pv == MAP_FAILED)
            {
                continue;
            }
            if (reinterpret_cast<uintptr_t>(pv) == cand)
            {
                sim().pvreg = static_cast<unsigned char*>(pv);
                break;
            }
            munmap(pv, SIM_PVREG_SPAN); // pre-4.17 kernels ignore the flag and relocate
        }
    }

    sigemptyset(&sim().irq_signals);
    sigaddset(&sim().irq_signals, SIGALRM);
    sigaddset(&sim().irq_signals, SIGUSR1);

    // Fault handler runs on its own stack (the faulting thread's stack may be
    // exactly what tripped the guard). Fixed size: SIGSTKSZ is not a compile
    // constant under glibc >= 2.34 with _GNU_SOURCE.
    static unsigned char altstack[64 * 1024];
    stack_t ss{};
    ss.ss_sp = altstack;
    ss.ss_size = sizeof(altstack);
    sigaltstack(&ss, nullptr);

    struct sigaction sa{};
    sa.sa_flags = SA_SIGINFO;
    sa.sa_mask = sim().irq_signals; // IRQs don't nest each other

    sa.sa_sigaction = on_sigalrm;
    sigaction(SIGALRM, &sa, nullptr);
    sa.sa_sigaction = on_sigusr1;
    sigaction(SIGUSR1, &sa, nullptr);

    struct sigaction fa{};
    fa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    // Block timer/device IRQs while reporting a fault: a deferred switch out of
    // the fault handler would abandon its alt-stack frame mid-report.
    fa.sa_mask = sim().irq_signals;
    fa.sa_sigaction = on_sigsegv;
    sigaction(SIGSEGV, &fa, nullptr);
    fa.sa_sigaction = on_sigill;
    sigaction(SIGILL, &fa, nullptr);

    // Graceful halt on Ctrl+C / kill.
    struct sigaction ta{};
    ta.sa_flags = SA_SIGINFO;
    ta.sa_sigaction = on_sigterm;
    sigaction(SIGINT, &ta, nullptr);
    sigaction(SIGTERM, &ta, nullptr);

    struct sigevent sev{};
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGALRM;
    if (timer_create(CLOCK_MONOTONIC, &sev, &sim().timer) == 0)
    {
        sim().timer_created = true;
    }
}

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
void arch_trace_stamp_id(struct arch_context* ctx, uint16_t id)
{
    sc(ctx)->tid = id;
}
#endif

// The host sim must EXIT on a fault/panic so CTest sees the status -- there is no
// LED and the blink terminal fallback (kernel.h) would spin forever. Replace it.
void kfault_terminate(void)
{
    arch_shutdown(132);
}

void arch_shutdown(int status)
{
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Mask IRQs/signals across the ENTIRE flush: root_entry -> arch_shutdown holds
    // no lock, so a timer ISR landing here would emit records AFTER the closing
    // SESSION's records_attempted snapshot (and could deferred-swap away from root
    // mid-drain), breaking the decoder's decoded+lost==attempted cross-check. Held
    // to _exit (never restored -- the process is dying).
    (void)arch_irq_save();
    // Emit the closing SESSION (far anchor + final count), then drain the ch1
    // telemetry ring to a file for the offline decoder. Best-effort: on a dying
    // path we still want whatever was captured. File is $KICKOS_TRACE_FILE or a
    // default in the CWD.
    kickos_trace_final_session();
    kickos_trace_report_counters();
    // report_counters printed the "[ktrace] counters" line via kprintf -> the
    // buffered console ring, but IRQs/signals are masked here so the SIGUSR1-driven
    // drain ISR can never run. Drain it synchronously (drain_sync pushes straight
    // to stdout, no signal needed) before _exit, else the line is stranded.
    console_tx_flush_sync();
    char const* path = getenv("KICKOS_TRACE_FILE");
    if (path == nullptr)
    {
        path = "kicktrace.bin";
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
    {
        char buf[512];
        while (true)
        {
            size_t got = kickos_rtt_ch1_drain(buf, sizeof(buf));
            if (got == 0)
            {
                break;
            }
            size_t off = 0;
            while (off < got)
            {
                ssize_t w = write(fd, buf + off, got - off);
                if (w <= 0)
                {
                    break;
                }
                off += static_cast<size_t>(w);
            }
        }
        close(fd);
    }
#endif
    _exit(status);
}

// Normal path: enqueue into the console ring; the SIGUSR1-driven drain ISR
// (console_tx_isr) writes it out. Before the ring is armed (early boot) and in
// ISR/panic/fault context, the routing guard in console.cc calls the sync writer
// below instead. Falls back to the sync writer itself until the ring is armed.
void arch_console_write(char const* buf, size_t n)
{
    console_tx_write(buf, n);
}

// The bounded synchronous stdout writer (panic / fault / pre-arm boot). The ring's
// prime/flush/overflow paths ultimately land here too, one byte at a time, via
// sim_tx_push.
void arch_console_write_sync(char const* buf, size_t n)
{
    size_t off = 0;
    while (off < n)
    {
        ssize_t w = write(1, buf + off, n - off);
        if (w < 0)
        {
            // A scheduling signal (SIGALRM/SIGUSR1, no SA_RESTART) can interrupt
            // the write; retry rather than truncate. Any other error is a dead
            // console -- nowhere left to report it, so stop.
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        if (w == 0)
        {
            break;
        }
        off += static_cast<size_t>(w);
    }
}

// Arch seam (console_tx.h): hand the kernel the ring storage + the emulated TX
// line so console_buffer_init binds console_tx_isr and arms the buffered path.
console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = g_tx_ring;
    *size = TX_RING_SIZE;
    *irq_line = TX_LINE;
    return &g_sim_tx_backend;
}

// --- Context / switching ---------------------------------------------------
void arch_context_init(struct arch_context* ctx,
                       void (*entry)(void*), void* arg,
                       void* stack_base, size_t stack_size,
                       int privileged)
{
    SimContext* c = sc(ctx);
    // The sim runs threads on host ucontexts, which need a host-sized stack -- an MCU-tuned
    // small caller stack (KICKOS_MIN_STACK_SIZE is a few hundred bytes) would overflow the
    // host. Substitute a host stack when the caller's is below the host floor; real HW uses
    // the caller's buffer directly (verified on silicon). Rare: the default spawn path hands
    // over the >=64K pool stack, so this malloc fires only for genuinely small caller stacks
    // (not freed -- the sim is a dev vehicle, not a small-caller-stack stress target).
    constexpr size_t SIM_HOST_MIN_STACK = 64 * 1024;
    if (stack_size < SIM_HOST_MIN_STACK)
    {
        stack_base = malloc(SIM_HOST_MIN_STACK);
        stack_size = SIM_HOST_MIN_STACK;
    }
    memset(c, 0, sizeof(*c));
    context_capture(&c->uc);
    // New threads always start with all interrupts enabled, independent of the
    // creating thread's current mask (which may be inside a critical section).
    sigemptyset(&c->uc.uc_sigmask);
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // ...except: start IRQ signals BLOCKED so no ISR can preempt between the
    // physical swap into this new thread and the trampoline's switch-in emit. The
    // trampoline unblocks them right after emitting (see trampoline()).
    sigaddset(&c->uc.uc_sigmask, SIGALRM);
    sigaddset(&c->uc.uc_sigmask, SIGUSR1);
#endif
    // getcontext() filled uc_stack with the CALLER's stack; retarget it here.
    c->uc.uc_stack.ss_sp = stack_base;
    c->uc.uc_stack.ss_size = stack_size;
    c->uc.uc_link = nullptr;
    c->entry = entry;
    c->arg = arg;
    // Privilege is modeled by the guard-page posture (per-task MPU regions +
    // the mid-syscall `raised` state), not stored here; the ARM port will map
    // `privileged` to CONTROL.nPRIV in the fabricated frame.
    (void)privileged;

    uintptr_t p = reinterpret_cast<uintptr_t>(c);
    unsigned hi = static_cast<unsigned>(p >> 32);
    unsigned lo = static_cast<unsigned>(p & 0xffffffffu);
    makecontext(&c->uc, reinterpret_cast<void (*)()>(trampoline), 2, hi, lo);
}

void arch_switch(struct arch_context* from, struct arch_context* to)
{
    SimContext* t = sc(to);
    if (sim().isr_depth > 0)
    {
        // Defer the physical swap to interrupt exit (PendSV analogue).
        sim().current = t;
        guard_apply_current();
        return;
    }
    SimContext* f = sc(from);
    sim().current = t;
    guard_apply_current();
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Synchronous swap site: physically-outgoing is `f`. Consumed on `t`'s resume.
    trace_switch_arm(f->tid);
#endif
    swapcontext(&f->uc, &t->uc);
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    trace_emit_switch_in(); // `f` resumed here on a later swap-in
#endif
}

void arch_start(struct arch_context* boot, struct arch_context* first)
{
    SimContext* b = sc(boot);
    SimContext* f = sc(first);
    memset(b, 0, sizeof(*b));
    getcontext(&b->uc);
    sim().current = f;
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // First switch: from = no-thread. `first` is new, so its trampoline emits it.
    trace_switch_arm(static_cast<uint16_t>(kickos::trace::TRACE_NO_THREAD));
#endif
    swapcontext(&b->uc, &f->uc);
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Symmetry with the other swap sites: consume on the resume side. `boot` does
    // not resume in practice (the system ends via arch_shutdown), and any pending
    // hand-off was already consumed by `first`'s trampoline, so this is a no-op --
    // but it keeps the invariant "every resume point consumes" uniform.
    trace_emit_switch_in();
#endif
}

// --- Critical section -------------------------------------------------------
arch_irq_state_t arch_irq_save(void)
{
    sigset_t prev;
    sigprocmask(SIG_BLOCK, &sim().irq_signals, &prev);
    // Encode whether SIGALRM was previously unblocked so restore is exact.
    arch_irq_state_t s = 0;
    if (not sigismember(&prev, SIGALRM))
    {
        s |= 1;
    }
    if (not sigismember(&prev, SIGUSR1))
    {
        s |= 2;
    }
    return s;
}

void arch_irq_restore(arch_irq_state_t state)
{
    sigset_t unblock;
    sigemptyset(&unblock);
    if (state & 1)
    {
        sigaddset(&unblock, SIGALRM);
    }
    if (state & 2)
    {
        sigaddset(&unblock, SIGUSR1);
    }
    if (state)
    {
        sigprocmask(SIG_UNBLOCK, &unblock, nullptr);
    }
}

int arch_in_isr(void)
{
    return sim().isr_depth > 0;
}

// --- Tickless clock + timer -------------------------------------------------
uint64_t arch_clock_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * kickos::KICKOS_NS_PER_SEC +
           static_cast<uint64_t>(ts.tv_nsec);
}

// Telemetry trace clock: microseconds as a u32 (wraps ~71 min). The sim has no
// cycle counter, so scale the monotonic ns clock down; us resolution is enough
// to time a host-emulated switch and keeps the SESSION-anchor math linear.
uint32_t arch_trace_now(void)
{
    return static_cast<uint32_t>(arch_clock_now() / 1000ull);
}

// The host has no silicon core clock; the sim reports throughput only, so there
// is no meaningful cycle->ns rate to expose. 0 == unknown (the ABI contract).
uint32_t arch_cpu_clock_hz(void)
{
    return 0;
}

void arch_timer_arm(uint64_t deadline_ns)
{
    if (not sim().timer_created)
    {
        return;
    }
    struct itimerspec its{};
    its.it_value.tv_sec = static_cast<time_t>(deadline_ns / kickos::KICKOS_NS_PER_SEC);
    its.it_value.tv_nsec = static_cast<long>(deadline_ns % kickos::KICKOS_NS_PER_SEC);
    // it_interval left zero -> one-shot.
    timer_settime(sim().timer, TIMER_ABSTIME, &its, nullptr);
}

void arch_timer_disarm(void)
{
    if (not sim().timer_created)
    {
        return;
    }
    struct itimerspec its{}; // all-zero disarms
    timer_settime(sim().timer, 0, &its, nullptr);
}

// --- MPU: mprotect over the user-RAM arena ---------------------------------
// Called at switch-in (switch_to, arch_start) for the INCOMING thread -- but while
// still executing on the OUTGOING thread's stack (arch_switch has not swapped yet).
// So this must NOT lower to the incoming resting posture here: that would PROT_NONE
// the outgoing thread's stack (not in the incoming set) and fault the swap itself,
// now that unprivileged stacks are arena-resident. Instead RECORD the incoming set
// and RAISE the arena (whole-RW, the privileged-switch background): the outgoing
// stack stays mapped through swapcontext, and the incoming thread's resting posture
// is applied on ITS OWN stack at its return-to-user boundary (arena_lower_to_applied
// from the syscall unwind, or the trampoline for a fresh thread). The regions pointer
// is the caller's TCB regions[] -- stable while the thread runs.
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n)
{
    if (sim().arena == nullptr)
    {
        return;
    }
    sim().applied = regions;
    sim().applied_n = n;
    arena_raise_all();
}

// Empty: arch_mpu_apply above already programs mprotect as it records, and the host
// switch is a synchronous longjmp. The symbol must still resolve, as the self-grant
// path calls it.
void kickos_arch_mpu_commit(void) {}

size_t arch_mpu_min_region(void)
{
    return static_cast<size_t>(sim().pagesize); // mprotect granularity
}

// mprotect governs only the mmap'd arena, so no real peripheral window is encodable
// here: fail closed. A sim driver test drives an arena-backed fake device (a data
// grant), never a real MMIO grant.
//
// The ONE admitted window is the sim's own fake register block, which is host pages
// mprotect describes exactly. It is what gives arch_periph_reg_write a DEV holder on
// the host; every other address, and every other shape of this one, still fails closed.
// The base is the one PUBLISHED at init, not a literal: nothing here depends on which
// candidate the host left free.
bool arch_mpu_region_encodable(uintptr_t base, size_t size)
{
    if (sim().pvreg == nullptr or sim().pagesize <= 0)
    {
        return false;
    }
    // Exact window, never a sub-range: the allowlist entry at SIM_PVREG_WINDOW is
    // refused by containment only while no wider window is grantable.
    if (base != reinterpret_cast<uintptr_t>(sim().pvreg) or size != SIM_PVREG_WINDOW)
    {
        return false;
    }
    // A host page coarser than the window leaves it undescribable by mprotect. Every
    // page size in practice divides SIM_PVREG_WINDOW, so this is a fail-closed floor
    // rather than a live condition.
    return SIM_PVREG_WINDOW % static_cast<size_t>(sim().pagesize) == 0;
}

// Privileged single-register write (arch.h). The caller's possession of the block at
// `base` and its containment of `[base + offset, +4)` are already checked in the
// syscall layer; this decides whether that exact register is on the allowlist above AND
// whether the value stays inside the entry's mask. The value is never trimmed and never
// read-modify-written: a silently dropped configuration bit is what a consumer's
// read-back exists to catch.
//
// This definition MUST stay in this TU. sim.cc is always extracted (it carries
// arch_init and the context switch) and is the FIRST member of kickos_arch_sim, so it
// resolves the symbol before common/arch_periph_reg_write_default.cc (in the same
// archive) can be pulled in. Moving it to a dedicated TU nothing else references would
// resolve the declining default instead, with no link error.
int arch_periph_reg_write(uintptr_t base, uintptr_t offset, uint32_t value)
{
    if (sim().pvreg == nullptr)
    {
        return -KOS_ENOSYS; // no fake block on this host: nothing to write
    }
    if (base != reinterpret_cast<uintptr_t>(sim().pvreg))
    {
        return -KOS_EINVAL; // the published base is the only one this backend tables
    }
    for (SimPrivWriteReg const& e : SIM_PRIV_WRITE_REGS)
    {
        if (e.offset == offset)
        {
            if ((value & ~e.mask) != 0)
            {
                return -KOS_EINVAL;
            }
            *reinterpret_cast<volatile uint32_t*>(base + offset) = value;
            return 0;
        }
    }
    return -KOS_EINVAL;
}

// mprotect takes an arbitrary page-aligned range, so no power-of-two size is needed.
int arch_mpu_region_pow2(void)
{
    return 0;
}

// Rule 7: the sim owns no MPU-governable peripheral (its "devices" are arena-backed
// fakes reached via a data grant), so it reserves nothing and only the arena /
// encodability rules apply.
size_t arch_reserved_blocks(struct arch_reserved_block* out, size_t max)
{
    (void)out;
    (void)max;
    return KICKOS_RESERVED_NONE;
}

// No Cortex-M bit-band on the host.
int arch_bitband_present(void)
{
    return 0;
}

uintptr_t arch_ram_base(void)
{
    return reinterpret_cast<uintptr_t>(sim().arena);
}

size_t arch_ram_size(void)
{
    return sim().arena_size;
}

void* arch_ram_alloc(size_t size)
{
    if (sim().arena == nullptr or size == 0)
    {
        return nullptr;
    }
    size_t const rsz = arch_ram_region_size(size);      // page multiple, >= one page
    size_t const ralign = arch_ram_region_align(size);  // one page here
    uintptr_t const base = reinterpret_cast<uintptr_t>(sim().arena);
    uintptr_t const cur = base + sim().arena_used;
    // Natural (absolute) alignment so one mprotect'd region covers the block;
    // subtract-form bounds are immune to the size_t wrap that (used + rsz) has.
    uintptr_t const aligned = (cur + (ralign - 1)) & ~static_cast<uintptr_t>(ralign - 1);
    size_t const off = static_cast<size_t>(aligned - base);
    if (aligned < cur or off > sim().arena_size or rsz > sim().arena_size - off)
    {
        return nullptr;
    }
    sim().arena_used = off + rsz;
    return reinterpret_cast<void*>(aligned);
}

// arch_domain_static_regions lives in kernel/domain/domain.cc (arch-independent).
// On the host/sim build the weak __kickos_code_*/__kickos_appdata_* linker symbols
// are undefined -> null -> it returns 0: the app's code/.data/.bss are host-process
// memory, not the mprotect'd arena, so the sim governs only the arena, never them.

// GNU ld default-script symbols bounding the host executable image (ELF header .. end
// of .bss). Array form + a uintptr_t decay dodges -Warray-compare.
extern "C" unsigned char __executable_start[];
extern "C" unsigned char _end[];

// The confused-deputy floor's read hook (arch.h). A string literal / thread name the
// app hands the kernel lives in the host binary's .text/.rodata/.data, NOT the
// mprotect'd arena -- no per-domain region can name it (unlike hardware, where the
// linker carves code/rodata as real MPU regions). Admit a range wholly inside the
// image and NOT touching the arena; reject a wild pointer outside both. CAVEAT: the
// image also holds the kernel's own code/.data (one binary), so this cannot separate
// app rodata from kernel statics -- the sim provably can't. The boundary it DOES
// enforce (a cross-domain arena read) stays closed: an arena range is disjoint from
// the image (separate anonymous mmap) and falls through to the region check.
bool arch_user_text_readable(uintptr_t ptr, size_t len)
{
    if (len == 0)
    {
        return true;
    }
    uintptr_t const end = ptr + len;
    if (end < ptr)
    {
        return false; // wrap
    }
    uintptr_t const istart = reinterpret_cast<uintptr_t>(__executable_start);
    uintptr_t const iend = reinterpret_cast<uintptr_t>(_end);
    if (iend <= istart or ptr < istart or end > iend)
    {
        return false;
    }
    uintptr_t const astart = reinterpret_cast<uintptr_t>(sim().arena);
    uintptr_t const aend = astart + sim().arena_size;
    bool const hits_arena = (sim().arena != nullptr and ptr < aend and end > astart);
    return not hits_arena;
}

// The write twin (arch.h). Needed even with KICKOS_HAVE_MPU=1: sim app globals live in
// the host image, not the arena, so arch_domain_static_regions models no static-data
// region here. Admission is the same rule as the read hook above: wholly inside the
// host image, clear of the arena.
//
// Two asymmetries vs hardware. An out-pointer into code/rodata is admitted (the two
// image-bound symbols cannot separate .text/.rodata from .data/.bss); the host's r-x /
// r-- page permissions backstop it. Kernel .data/.bss also sit inside the image on rw
// pages, so an unprivileged out-pointer aimed at kernel state is admitted with NO
// backstop; the sim enforces only the arena cross-domain boundary.
bool arch_user_data_writable(uintptr_t ptr, size_t len)
{
    return arch_user_text_readable(ptr, len);
}

uintptr_t arch_mpu_probe_addr(void)
{
    return reinterpret_cast<uintptr_t>(sim().guard);
}

// --- Syscall trap -----------------------------------------------------------
uintptr_t arch_syscall(uintptr_t nr,
                       uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    // Emulated privilege raise, tracked PER-CONTEXT via SimContext::raised so it
    // survives a blocking switch: kernel dispatch may touch any user memory, so
    // the whole arena is granted for the duration. If the dispatch blocks and we
    // later resume this thread mid-syscall, guard_apply_current() re-raises (the
    // switch-in's arch_mpu_apply would otherwise reinstate the caller's resting
    // posture while it is still running kernel code). On the final unwind we drop
    // back to exactly the caller's resting region set (sim().applied).
    // Cache the calling context: a thread only ever runs as its own context, so
    // the same SimContext is current at entry and (after any blocking round-trip)
    // at exit -- pairing the raise and unwind on `self` makes that explicit.
    SimContext* self = nullptr;
    if (sim().arena != nullptr and sim().current != nullptr)
    {
        self = sim().current;
        self->raised++;
        arena_raise_all();
    }
    uintptr_t r = syscall_dispatch(nr, a0, a1, a2, a3);
    if (self != nullptr)
    {
        self->raised--;
        if (self->raised == 0)
        {
            arena_lower_to_applied();
        }
    }
    return r;
}

// --- Interrupt controller (mask / unmask / raise) --------------------------
// Width of the irq_masked bitset: lines >= this are never maskable (a raise
// always delivers). A board needing more than 32 lines must widen irq_masked.
enum
{
    SIM_IRQ_LINES = 32
};

// Self-bracketed (arch_irq_save/restore) so the irq_masked/irq_pending RMWs are
// atomic against a device ISR regardless of the caller: kos_irq_inject/unmask reach
// here without an IrqLock (syscall.cc), and a bare RMW preempted mid-update would
// write back a stale mask -> re-enable a mid-service line -> phantom wake.
void arch_irq_mask(int line)
{
    if (line < 0 or line >= SIM_IRQ_LINES)
    {
        return;
    }
    arch_irq_state_t s = arch_irq_save();
    sim().irq_masked |= static_cast<int>(1u << line);
    arch_irq_restore(s);
}

void arch_irq_unmask(int line)
{
    if (line < 0 or line >= SIM_IRQ_LINES)
    {
        return;
    }
    arch_irq_state_t s = arch_irq_save();
    sim().irq_masked &= static_cast<int>(~(1u << line));
    // Latch-and-coalesce: a raise taken while the line was masked redelivers now,
    // through the ISR path (raise(SIGUSR1) -> on_sigusr1 -> kickos_isr_irq), the way
    // the NVIC pend-fires on enable. The raise pends under this bracket (SIGUSR1
    // blocked) and lands at its release -- the outer IrqLock release if the caller
    // holds one -- in ISR context, never a direct post here.
    if (static_cast<unsigned>(sim().irq_pending) & (1u << line))
    {
        sim().irq_pending &= static_cast<int>(~(1u << line));
        sim().pending_irq = line;
        raise(SIGUSR1);
    }
    arch_irq_restore(s);
}

void arch_irq_clear_pending(int line)
{
    if (line < 0 or line >= SIM_IRQ_LINES)
    {
        return;
    }
    arch_irq_state_t s = arch_irq_save();
    sim().irq_pending &= static_cast<int>(~(1u << line));
    arch_irq_restore(s);
}

void arch_irq_inject(int irq)
{
    arch_irq_state_t s = arch_irq_save();
    // Latch-and-coalesce: a raise on a masked in-range line sets the one-deep pending
    // bit (redelivered at unmask), NOT dropped. An unmasked line -- or a never-maskable
    // line >= SIM_IRQ_LINES -- delivers now (the raise pends under this bracket and
    // lands at its release, in ISR context).
    if (irq >= 0 and irq < SIM_IRQ_LINES and (static_cast<unsigned>(sim().irq_masked) & (1u << irq)))
    {
        sim().irq_pending |= static_cast<int>(1u << irq);
    }
    else
    {
        sim().pending_irq = irq;
        raise(SIGUSR1);
    }
    arch_irq_restore(s);
}

// --- Idle -------------------------------------------------------------------
void arch_idle_wait(void)
{
    sigset_t empty;
    sigemptyset(&empty);
    sigsuspend(&empty); // atomically unblock + wait for a signal
}
}
