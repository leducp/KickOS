// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/arch/arch.h>

#include <kickos/arch/clk_q32.h> // KICKOS_NS_PER_SEC (canonical 1e9 ns/sec)
#include <kickos/console_tx.h>
#include <kickos/instance_local.h>
#include <kickos/sys/atomic.h>

#include <fatal_status.ld.h>

#include <new> // placement new (arch_context_init)

#include <ucontext.h>
#include <signal.h>
#include <sys/syscall.h>
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

namespace kickos
{
    void kprintf(char const* fmt, ...) __attribute__((format(printf, 1, 2)));
#if defined(KICKOS_MULTI_INSTANCE) && KICKOS_MULTI_INSTANCE
    int kmain(int argc, char** argv);
#endif
}
extern "C" void kpanic_enter(void);
extern "C" void kfault_terminate(void);

namespace
{
    using kickos::Atomic;
    using kickos::Order;

    // --- Internal context layout over the opaque arch_context storage ----------
    struct SimContext
    {
        ucontext_t uc;
        void (*entry)(void*);
        void* arg;
        Atomic<int, Order::RELAXED> raised; // >0 while mid-syscall (read from signal-driven switch)
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
    // (plain -Os is clean, -Os plus UBSan is not). noinline is load-bearing: it keeps the
    // call out of arch_context_init, whose stack parameters are live across it.
    __attribute__((noinline)) void context_capture(ucontext_t* uc)
    {
        getcontext(uc);
    }

#if defined(KICKOS_MULTI_INSTANCE) && KICKOS_MULTI_INSTANCE
    // Longest line held before it is emitted unterminated. A cap, not a limit on output:
    // a longer line is simply split across two writes.
    enum { SIM_OUT_LINE_MAX = 240 };
#endif

    // Several sim backends (one per emulated MCU / KickCAT slave) co-reside in one
    // host process, so this state is instance-scoped and never a plain global.
    struct SimInstance
    {
        // --- running-context tracking + deferred-switch state ---
        SimContext* current = nullptr; // arch's view of the running ctx
        Atomic<sig_atomic_t, Order::RELAXED> isr_depth = 0;

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

        // The running thread's resting region set, so the syscall raise lowers back
        // to exactly it. This is the caller's own TCB regions[], valid only while
        // that thread runs.
        struct arch_mpu_region const* applied = nullptr;
        size_t applied_n = 0;

        // --- emulated device IRQ hand-off (async-signal to ISR) ---
        Atomic<sig_atomic_t, Order::RELAXED> pending_irq = -1;
        // bit L set => line L masked (a raise latches, see irq_pending). All lines
        // start MASKED at reset (the arch.h reset contract); a driver unmasks its
        // line (arch_irq_unmask, or irq_claim) before use.
        Atomic<sig_atomic_t, Order::RELAXED> irq_masked = static_cast<sig_atomic_t>(0xFFFFFFFFu);
        // bit L set => a raise landed on line L while it was masked (latched one-
        // deep, coalesced). Redelivered through the ISR path at unmask.
        Atomic<sig_atomic_t, Order::RELAXED> irq_pending = 0;

        // --- emulated buffered-console TX-empty interrupt source ---
        // tx_enabled is the peripheral-level enable, tx_asserted the level line.
        // tx_budget is a SYNTHETIC per-ISR-delivery slot budget: without it the ring would
        // drain in one shot and never fill or wrap. It constrains slot_free ONLY inside the
        // drain ISR, so the synchronous prime/flush/overflow paths never stall.
        Atomic<sig_atomic_t, Order::RELAXED> tx_enabled = 0;
        Atomic<sig_atomic_t, Order::RELAXED> tx_asserted = 0;
        Atomic<sig_atomic_t, Order::RELAXED> in_tx_isr = 0; // scopes the synthetic budget to the ISR
        int tx_budget = 0;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
        // Consume-once switch-emit hand-off. The physically-outgoing tid is armed
        // just before each ucontext swap; whichever context RESUMES (an existing
        // one right after its swapcontext, or a new one at the trampoline) consumes
        // it and emits the SWITCH record for {from -> current}.
        Atomic<sig_atomic_t, Order::RELAXED> switch_pending = 0;
        uint16_t switch_from = 0xFFFF;
#endif

#if defined(KICKOS_MULTI_INSTANCE) && KICKOS_MULTI_INSTANCE
        // Where arch_shutdown goes instead of ending the process: the host frame that
        // started this instance. _exit would take every co-resident instance with it.
        ucontext_t exit_uc;
        int entry_argc = 0;
        char** entry_argv = nullptr;
        int exit_status = 0;
        bool exit_armed = false;

        // One host stdout serves every instance and this backend emits a byte at a time,
        // so a line is held here and emitted in one write or it comes out shredded.
        char out_line[SIM_OUT_LINE_MAX];
        unsigned out_len = 0;
#endif
    };

    // All-constant init keeps this in BSS. Signal handlers read it, so the selector must
    // stay async-signal-safe: never a lazy TLS call (instance_local.h).
    kickos::InstanceLocal<SimInstance> g_sim;
    SimInstance& sim()
    {
        return g_sim.get();
    }

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // `from_tid` is the context being swapped AWAY from (the physically-outgoing side).
    void trace_switch_arm(uint16_t from_tid)
    {
        sim().switch_from = from_tid;
        sim().switch_pending = 1;
    }
    // Consume the hand-off on the RESUMING side and emit {from -> current}. Called
    // right after every swapcontext and at the trampoline (a new thread's first run).
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
    // The first candidate that maps is published as sim().pvreg and is the only base the seam
    // and the encoder answer for.
    // user/apps/common/selftest walks the SAME list in the SAME order. Keep them identical.
    constexpr uintptr_t SIM_PVREG_BASES[] = {
        0x40000000u,          // 1 GiB
        0x100000000ull,       // 4 GiB
        0x400000000ull,       // 16 GiB
        0x10000000000ull,     // 1 TiB
        0x100000000000ull,    // 16 TiB
    };
    // 64 KiB: every host page size in practice (4/16/64 KiB) divides it, so the
    // "mprotect can describe this exactly" precondition below holds by construction.
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
        // its switch-in record before running its body.
        trace_emit_switch_in();
#endif
        // arch_context_init blocked the IRQ signals; this is the matching unblock, and
        // it must precede the arena transition below, whose arch_irq_restore is exact
        // and would otherwise re-block them for this thread's whole life.
        sigprocmask(SIG_UNBLOCK, &sim().irq_signals, nullptr);
        // Enter user code under this thread's OWN resting MPU posture: arch_mpu_apply at the
        // starting switch left the arena raised, so lower to it here, on this thread's own
        // stack, which the gap-based lower keeps mapped. Mask the emulated IRQ lines across the
        // transition so no ISR reprograms the arena mid-way.
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

    // Privileged posture: whole arena accessible (the background-region analog), used
    // while KERNEL code runs. An unprivileged thread's own stack is an arena block, so
    // kernel code running ON that stack MUST keep it mapped.
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
    // Lower the arena from the raised (whole-RW) posture to the running thread's resting region
    // set WITHOUT ever transiently unmapping a granted region: the caller runs on its own
    // arena-resident stack, so a window with that stack unmapped faults on the next push. The
    // arena being already whole-RW, PROT_NONE only the GAPS between granted regions, then set
    // each region to its exact attr (an RW->RO change never unmaps).
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
        // Cap above KICKOS_MPU_MAX_REGIONS (8); extra entries are clamped.
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
        if (sim().arena != nullptr and sim().current != nullptr
            and sim().current->raised > 0)
        {
            arena_raise_all();
        }
    }

    // isr_frame_leave performs, at depth 0, any context switch deferred during the ISR.
    void isr_frame_enter()
    {
        sim().isr_depth = sim().isr_depth + 1;
    }
    void isr_frame_leave(SimContext* interrupted)
    {
        sim().isr_depth = sim().isr_depth - 1;
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
        // Deliberately small, so ordinary console traffic WRAPS the ring and crosses
        // the index-mask boundary many times. Usable 127 still exceeds the largest
        // single burst, so a burst takes the fast enqueue+prime path.
        TX_RING_SIZE = 128,  // power of two (index masking); usable capacity 127
        TX_BUDGET = 8       // bytes drained per ISR delivery (synthetic slot budget)
    };
    kickos::InstanceLocal<char[TX_RING_SIZE]> g_tx_ring;

    int sim_tx_slot_free()
    {
        // Sync paths (prime/flush/overflow) must never stall; the budget bites only
        // in ISR context.
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

#if defined(KICKOS_MULTI_INSTANCE) && KICKOS_MULTI_INSTANCE
    // Process wide, not instance local: it states whether this stdout is shared. A lone
    // instance emits what a build without the knob emits, byte for byte, so every gate
    // that matches a banner or a TAP line exactly holds with the knob compiled in.
    bool g_out_tagged = false;

    // ONE write is the whole of what keeps a line intact against the co-residents sharing
    // this fd. Async-signal-safe throughout (the tag is formed by hand, not printf), so
    // the drain ISR may call it.
    void sim_line_flush()
    {
        SimInstance& s = sim();
        if (s.out_len == 0)
        {
            return;
        }
        char buf[8 + SIM_OUT_LINE_MAX];
        unsigned n = 0;
        if (g_out_tagged)
        {
            unsigned const idx = kickos_instance_index();
            buf[n++] = '[';
            if (idx >= 100u)
            {
                buf[n++] = static_cast<char>('0' + (idx / 100u) % 10u);
            }
            if (idx >= 10u)
            {
                buf[n++] = static_cast<char>('0' + (idx / 10u) % 10u);
            }
            buf[n++] = static_cast<char>('0' + idx % 10u);
            buf[n++] = ']';
            buf[n++] = ' ';
        }
        for (unsigned i = 0; i < s.out_len; i++)
        {
            buf[n++] = s.out_line[i];
        }
        // Cleared BEFORE the write: a fault inside it must not leave the line to be
        // emitted a second time by the shutdown flush.
        s.out_len = 0;
        unsigned off = 0;
        while (off < n)
        {
            ssize_t w = write(1, buf + off, n - off);
            if (w < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                break;
            }
            off += static_cast<unsigned>(w);
        }
    }

    void sim_line_put(char c)
    {
        SimInstance& s = sim();
        s.out_line[s.out_len] = c;
        s.out_len++;
        if (c == '\n' or s.out_len == SIM_OUT_LINE_MAX)
        {
            sim_line_flush();
        }
    }
#endif

    void sim_tx_push(uint8_t b)
    {
        if (sim().in_tx_isr != 0 and sim().tx_budget > 0)
        {
            sim().tx_budget--;
        }
        char c = static_cast<char>(b);
#if defined(KICKOS_MULTI_INSTANCE) && KICKOS_MULTI_INSTANCE
        sim_line_put(c);
#else
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
#endif
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

    // Re-asserts the level line while the peripheral IRQ is still enabled, so the next
    // SIGUSR1 continues the drain. console_tx_isr calls irq_disable when the ring
    // empties, which clears tx_enabled and ends the chain. Bounded: every delivery
    // drains >= TX_BUDGET bytes from a finite ring.
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

    // The seam is handed the ucontext alone, so the siginfo facts travel here instead:
    // written by a fault handler immediately before it calls kickos_fault_kill_thread,
    // read only inside that call.
    struct SimFaultInfo
    {
        uintptr_t addr;
        int code;
        bool addr_valid;
    };
    kickos::InstanceLocal<SimFaultInfo> g_sim_fault = {};

    SimFaultInfo& sim_fault()
    {
        return g_sim_fault.get();
    }

    void on_sigsegv(int, siginfo_t* si, void* ucontext)
    {
        uintptr_t addr = reinterpret_cast<uintptr_t>(si->si_addr);
        // BEFORE isr_frame_enter: arch_fault_is_user_thread reads isr_depth to tell a
        // fault in an ISR body (a kernel bug) from one in the thread it interrupted, and
        // isr_frame_enter below would make every fault look like the former.
        sim_fault() = {addr, si->si_code, true};
        if (kickos_fault_kill_thread(ucontext))
        {
            return; // sigreturn resumes at kickos_thread_fault_exit
        }
        // Establish ISR context: kickos_isr_fault reports via kprintf, and the console
        // routing guard MUST see arch_in_isr() to take the synchronous writer, else the
        // fault line is enqueued into the buffered ring and lost when this handler
        // _exit()s without draining it.
        isr_frame_enter();
        // si_code and the faulting PC, which kickos_isr_fault's shared banner cannot
        // carry. Without them the banner is actively misleading: a resumed-garbage
        // context traps on an instruction FETCH at 0, and reads identically to a null
        // store. SI_KERNEL with pc == 0 is the tell, and si_addr alone cannot show it.
        uintptr_t pc = 0;
#if defined(__x86_64__)
        if (ucontext != nullptr)
        {
            pc = static_cast<uintptr_t>(
                static_cast<ucontext_t*>(ucontext)->uc_mcontext.gregs[REG_RIP]);
        }
#else
        (void)ucontext;
#endif
        ::kickos::kprintf("\n[sim] SIGSEGV si_code=%d si_addr=%p pc=%p\n", si->si_code,
                          reinterpret_cast<void*>(addr), reinterpret_cast<void*>(pc));
        // Read vs write is not distinguishable portably here; reported as a write.
        kickos_isr_fault(addr, 1);
        // kickos_isr_fault is expected to terminate or recover; if it returns we
        // cannot safely resume the faulting store, so halt.
        arch_shutdown(2);
    }

    // Illegal instruction (host: x86 `ud2` from __builtin_trap): the sim's CPU-fault
    // reporter. kpanic_enter masks signals, forces the synchronous polled writer and
    // flushes the ring, so the dump survives the ARMED console ring instead of being
    // enqueued and lost.
    void on_sigill(int, siginfo_t* si, void* ucontext)
    {
        // si_addr is the faulting PC here, not a data address.
        sim_fault() = {0, si->si_code, false};
        if (kickos_fault_kill_thread(ucontext))
        {
            return;
        }
        isr_frame_enter();
        kpanic_enter();
        ::kickos::kprintf("\n=== SIM FAULT (illegal instruction) at %p ===\n", si->si_addr);
        kfault_terminate(); // -> arch_shutdown(KICKOS_FATAL_STATUS)
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

    // The user-RAM arena the MPU emulation governs. It must fit the whole thread pool at once
    // on top of the domain-data allocs and the probe page. No kernel config header here, so the
    // size cannot be derived.
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

    // The fake write-PV-only register block, OUTSIDE the arena: it must not be reachable as a
    // RAM grant, and arena_lower_to_applied must skip it. MAP_FIXED_NOREPLACE never evicts an
    // existing mapping, so a candidate this process already uses is skipped. Kept if already
    // mapped: a second arch_init must not drop the block the published base and every live DEV
    // grant still refer to.
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

    // Fault handler runs on its own stack (the faulting thread's stack may be exactly what
    // tripped the guard). Fixed size: SIGSTKSZ is not a compile constant under glibc >= 2.34
    // with _GNU_SOURCE. Per HOST THREAD: sigaltstack is a per-thread POSIX property, so one
    // shared block is corrupted the moment two host threads fault at once.
#if defined(KICKOS_MULTI_INSTANCE) && KICKOS_MULTI_INSTANCE
    static __thread unsigned char altstack[64 * 1024];
#else
    static unsigned char altstack[64 * 1024];
#endif
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

    struct sigaction ta{};
    ta.sa_flags = SA_SIGINFO;
    ta.sa_sigaction = on_sigterm;
    sigaction(SIGINT, &ta, nullptr);
    sigaction(SIGTERM, &ta, nullptr);

    struct sigevent sev{};
    sev.sigev_signo = SIGALRM;
#if defined(KICKOS_MULTI_INSTANCE) && KICKOS_MULTI_INSTANCE
    // SIGEV_SIGNAL generates a PROCESS-directed signal, which the kernel hands to any
    // thread not blocking it: one instance's tick would then be serviced on another's
    // host thread, and the instance that armed it would never wake. SIGEV_THREAD_ID is
    // what binds a timer to the thread that owns the instance.
    sev.sigev_notify = SIGEV_THREAD_ID;
    // Spelled through the union member: glibc publishes POSIX names for the SIGEV_THREAD
    // arm of sigevent but none for the tid, so sigev_notify_thread_id does not exist here.
    sev._sigev_un._tid = static_cast<pid_t>(syscall(SYS_gettid));
#else
    sev.sigev_notify = SIGEV_SIGNAL;
#endif
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

// The host sim must EXIT on a fault/panic so CTest sees the status: there is no LED
// and the blink terminal fallback (kernel.h) would spin forever.
void kfault_terminate(void)
{
    // Resolves to the no-op fallback: the sync writer returns only once the host has
    // taken every byte.
    arch_console_flush_sync();
    arch_shutdown(KICKOS_FATAL_STATUS);
}

void arch_shutdown(int status)
{
    // Mask IRQs/signals for the rest of this function: root_entry -> arch_shutdown holds no
    // lock, so a timer ISR landing here could deferred-swap away from root mid-drain and strand
    // whatever is still in the ring. Held to _exit. Under telemetry it also keeps a late ISR
    // from emitting records after the closing SESSION's records_attempted snapshot.
    (void)arch_irq_save();
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // The closing SESSION (far anchor + final count). MUST precede the console drain
    // below, because report_counters prints through the buffered ring.
    kickos_trace_final_session();
    kickos_trace_report_counters();
#endif
    // Without this, anything still enqueued at shutdown is stranded by _exit. Safe
    // here because IRQs/signals are masked, so the SIGUSR1-driven drain ISR can never
    // run. No-op while the ring is unarmed.
    console_tx_flush_sync();
#if defined(KICKOS_MULTI_INSTANCE) && KICKOS_MULTI_INSTANCE
    sim_line_flush(); // whatever the app left without a trailing newline
#endif
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Drain the ch1 telemetry ring to a file for the offline decoder. Path is
    // $KICKOS_TRACE_FILE or a default in the CWD.
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
#if defined(KICKOS_MULTI_INSTANCE) && KICKOS_MULTI_INSTANCE
    // Ending THIS instance, not the process: the host frame that started it resumes with
    // the status, and its co-residents keep running. setcontext returns only on failure,
    // so the _exit below stays reachable as the loud fallback rather than as dead code.
    if (sim().exit_armed)
    {
        sim().exit_status = status;
        setcontext(&sim().exit_uc);
    }
#endif
    _exit(status);
}

#if defined(KICKOS_MULTI_INSTANCE) && KICKOS_MULTI_INSTANCE
// How many kernels this process hosts. Must be settled before the first instance thread:
// it is read by every console flush, including one from a signal handler.
void arch_sim_instance_hosting(unsigned count)
{
    g_out_tagged = count > 1u;
}

// Bring up the instance already selected on this host thread and run its kernel.
//
// getcontext MUST be called here and not through the file's noinline wrapper: a resumed context
// returns through the return address ON THE STACK, so the capturing frame must still be intact.
// The arguments are parked in the instance so nothing lives across the returns-twice call.
int arch_sim_instance_run(int argc, char** argv)
{
    sim().entry_argc = argc;
    sim().entry_argv = argv;
    getcontext(&sim().exit_uc);
    if (sim().exit_armed)
    {
        // Resumed from arch_shutdown, which restored the capture-time signal mask along
        // with everything else. Nothing on this host thread may take this instance's
        // interrupts from here on, and its timer would otherwise still be armed.
        sigprocmask(SIG_BLOCK, &sim().irq_signals, nullptr);
        if (sim().timer_created)
        {
            timer_delete(sim().timer);
            sim().timer_created = false;
        }
        return sim().exit_status;
    }
    sim().exit_armed = true;
    arch_init();
    return kickos::kmain(sim().entry_argc, sim().entry_argv);
}
#endif

// Enqueue into the console ring; the SIGUSR1-driven drain ISR writes it out. Before the ring
// is armed, and in ISR/panic/fault context, console.cc's routing guard takes the sync writer.
void arch_console_write(char const* buf, size_t n)
{
    console_tx_write(buf, n);
}

// The bounded synchronous stdout writer (panic / fault / pre-arm boot). The ring's
// prime/flush/overflow paths land here too, one byte at a time, via sim_tx_push.
void arch_console_write_sync(char const* buf, size_t n)
{
#if defined(KICKOS_MULTI_INSTANCE) && KICKOS_MULTI_INSTANCE
    for (size_t i = 0; i < n; i++)
    {
        sim_line_put(buf[i]);
    }
    // Panic and fault reach here, and neither returns to a newline, so an unterminated
    // tail would otherwise be lost.
    sim_line_flush();
#else
    size_t off = 0;
    while (off < n)
    {
        ssize_t w = write(1, buf + off, n - off);
        if (w < 0)
        {
            // A scheduling signal (SIGALRM/SIGUSR1, no SA_RESTART) can interrupt
            // the write; retry rather than truncate. Any other error is a dead
            // console: nowhere left to report it, so stop.
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
#endif
}

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = g_tx_ring.get();
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
    // Host ucontexts need a host-sized stack: an MCU-tuned caller stack
    // (KICKOS_MIN_STACK_SIZE is a few hundred bytes) would overflow the host, so a host
    // stack is substituted below that floor. Never freed.
    constexpr size_t SIM_HOST_MIN_STACK = 64 * 1024;
    if (stack_size < SIM_HOST_MIN_STACK)
    {
        stack_base = malloc(SIM_HOST_MIN_STACK);
        stack_size = SIM_HOST_MIN_STACK;
    }
    // Value-initialised in place, not memset: SimContext holds atomics.
    new (static_cast<void*>(c)) SimContext{};
    context_capture(&c->uc);
    // Start from an empty mask, not the creating thread's (which may be inside a
    // critical section), so a new thread's posture never depends on its spawner.
    sigemptyset(&c->uc.uc_sigmask);
    // The IRQ signals stay blocked until the trampoline unblocks them, which is the first point
    // at which this thread runs on its own stack.
    //
    // glibc's swapcontext installs the TARGET's sigmask before it loads the target's rsp, and
    // arch_switch publishes sim().current before the swap. With an empty mask here, a SIGALRM
    // in that gap runs on the OUTGOING thread's stack while sim().current already names this
    // one, so isr_frame_leave saves the outgoing frame into THIS context and destroys the
    // makecontext entry.
    sigaddset(&c->uc.uc_sigmask, SIGALRM);
    sigaddset(&c->uc.uc_sigmask, SIGUSR1);
    // getcontext() filled uc_stack with the CALLER's stack; retarget it here.
    c->uc.uc_stack.ss_sp = stack_base;
    c->uc.uc_stack.ss_size = stack_size;
    c->uc.uc_link = nullptr;
    c->entry = entry;
    c->arg = arg;
    // Privilege is modeled by the guard-page posture (per-task MPU regions + the
    // mid-syscall `raised` state), not stored here.
    (void)privileged;

    uintptr_t p = reinterpret_cast<uintptr_t>(c);
    unsigned hi = static_cast<unsigned>(p >> 32);
    unsigned lo = static_cast<unsigned>(p & 0xffffffffu);
    makecontext(&c->uc, reinterpret_cast<void (*)()>(trampoline), 2, hi, lo);
}

void arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                       void* stack_base, size_t stack_size)
{
    SimContext* c = sc(ctx);
    // (1) REUSE the host stack this context already runs on. arch_context_init mallocs a
    // 64 KiB substitute for a caller buffer below SIM_HOST_MIN_STACK and never frees it,
    // so re-deriving from the caller's fields would leak one per rebuild.
    void* const host_base = c->uc.uc_stack.ss_sp;
    size_t const host_size = c->uc.uc_stack.ss_size;
    if (host_base != nullptr and host_size != 0)
    {
        stack_base = host_base;
        stack_size = host_size;
    }
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // arch_context_init re-initialises the whole SimContext, and the trace id is stamped
    // once at thread_create; without this the rebuilt thread switches in as an unknown tid.
    uint16_t const tid = c->tid;
#endif
    arch_context_init(ctx, entry, nullptr, stack_base, stack_size, 1);
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    c->tid = tid;
#endif
    // (2) RAISE. Privilege here is the guard-page posture, arch_context_init discarding its
    // `privileged` argument. arch_switch programs the thread's RESTING grant on switch-in, so a
    // stub resumed through the shared body alone would SIGSEGV on its first read of kernel
    // state. The count is what makes guard_apply_current hold the arena up across the teardown's
    // blocking points; never unwound, the stub never returning. This context is not the running
    // one, so its switch-in raises and arena_raise_all() is not called here.
    c->raised = c->raised + 1;
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
    new (static_cast<void*>(b)) SimContext{};
    getcontext(&b->uc);
    sim().current = f;
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // First switch: from = no-thread. `first` is new, so its trampoline emits it.
    trace_switch_arm(static_cast<uint16_t>(kickos::trace::TRACE_NO_THREAD));
#endif
    swapcontext(&b->uc, &f->uc);
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Keeps the invariant "every resume point consumes" uniform; `boot` does not resume in
    // practice.
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

// Telemetry trace clock: microseconds as a u32 (wraps ~71 min). The sim has no cycle
// counter, so the monotonic ns clock is scaled down.
uint32_t arch_trace_now(void)
{
    return static_cast<uint32_t>(arch_clock_now() / 1000ull);
}

// No host core clock to report: 0 == unknown (the ABI contract).
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
// Called at switch-in (switch_to, arch_start) for the INCOMING thread while still executing on
// the OUTGOING thread's stack, arch_switch not having swapped yet. THIS MUST NOT LOWER TO THE
// INCOMING RESTING POSTURE: that would PROT_NONE the outgoing thread's arena-resident stack and
// fault the swap itself. It RECORDS the incoming set and RAISES the arena, and the incoming
// thread's resting posture is applied on ITS OWN stack at its return-to-user boundary
// (arena_lower_to_applied, or the trampoline for a fresh thread). The regions pointer is the
// caller's TCB regions[], stable while the thread runs.
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n,
                    struct arch_mpu_encoded const* image)
{
    (void)image;
    if (sim().arena == nullptr)
    {
        return;
    }
    sim().applied = regions;
    sim().applied_n = n;
    arena_raise_all();
}

// mprotect takes the addresses themselves, so there is nothing to pre-encode: the image
// records only which regions the arena can enforce.
uint32_t arch_mpu_encode(struct arch_mpu_region const* regions, size_t n,
                         struct arch_mpu_encoded* out)
{
    if (n > ARCH_MPU_ENCODED_SLOTS)
    {
        n = ARCH_MPU_ENCODED_SLOTS;
    }
    uint32_t seated = 0;
    for (size_t i = 0; i < n; i++)
    {
        if (arena_region_valid(regions[i].base, regions[i].size))
        {
            seated |= static_cast<uint32_t>(1) << i;
        }
    }
    out->seated = seated;
    return seated;
}

// Empty: arch_mpu_apply above already programs mprotect as it records. The symbol must still
// resolve, the self-grant path calling it.
void kickos_arch_mpu_commit(void) {}

// --- Fault isolation --------------------------------------------------------
// arch_syscall raises the WHOLE arena for the duration of dispatch and tracks it per-context in
// SimContext::raised, so `raised == 0` at fault time carries what CONTROL.nPRIV carries on ARM:
// user code, not kernel code the thread merely called. isr_depth covers the other half of
// clause 3.2.
//
// The resume context is the ucontext the HOST kernel wrote, on a dedicated sigaltstack, so it is
// complete by construction and no stack-bounds test applies. A sim thread's stack is a plain
// arena block with no guard page, so a guest stack overflow is undetected on this backend.
bool arch_fault_is_user_thread(void* frame)
{
#if defined(__x86_64__)
    if (frame == nullptr)
    {
        return false;
    }
    if (sim().isr_depth != 0)
    {
        return false;
    }
    if (sim().current == nullptr or sim().current->raised != 0)
    {
        return false;
    }
    return true;
#else
    (void)frame;
    return false;
#endif
}

void arch_fault_redirect_to_exit(void* frame)
{
#if defined(__x86_64__)
    ucontext_t* const uc = static_cast<ucontext_t*>(frame);
    uintptr_t const pc = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RIP]);
    SimFaultInfo const& f = sim_fault();
    kickos_fault_record("si_code", static_cast<uint32_t>(f.code), pc, f.addr,
                        static_cast<int>(f.addr_valid));
    // The stub is kernel code and cap_teardown reaches memory outside this thread's
    // resting grant, so raise the arena as arch_syscall does. Never unwound: the stub
    // never returns, and the raised count is what makes guard_apply_current hold the
    // arena up across the teardown's blocking points.
    if (sim().current != nullptr)
    {
        sim().current->raised = sim().current->raised + 1;
        arena_raise_all();
    }
    uc->uc_mcontext.gregs[REG_RIP] =
        static_cast<greg_t>(reinterpret_cast<uintptr_t>(&kickos_thread_fault_exit));
    // The stub runs at the top of the dying thread's stack. Guarded on the faulting RSP lying
    // inside the Thread's recorded stack, because arch_context_init substitutes a host stack for
    // a caller buffer below SIM_HOST_MIN_STACK and those fields then do not describe the stack
    // in use. System V AMD64 wants rsp+8 16-byte aligned at the callee's first instruction, and
    // this jump skips the call that would have pushed the return address, so the slot is
    // reserved here and filled with a return address that cannot be taken.
    uintptr_t const rsp = static_cast<uintptr_t>(uc->uc_mcontext.gregs[REG_RSP]);
    uintptr_t const top = kickos_fault_stack_top();
    if (top != 0 and kickos_fault_frame_trusted(reinterpret_cast<void const*>(rsp), 0))
    {
        uintptr_t const sp = (top & ~static_cast<uintptr_t>(15)) - 8;
        *reinterpret_cast<uintptr_t*>(sp) = 0;
        uc->uc_mcontext.gregs[REG_RSP] = static_cast<greg_t>(sp);
    }
#else
    (void)frame;
#endif
}

size_t arch_mpu_min_region(void)
{
    return static_cast<size_t>(sim().pagesize); // mprotect granularity
}

// mprotect governs only the mmap'd arena, so the ONE admitted window is the sim's own fake
// register block; everything else fails closed. The base is the one PUBLISHED at init, not a
// literal.
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
    // A host page coarser than the window leaves it undescribable by mprotect. A fail-closed
    // floor: every page size in practice divides SIM_PVREG_WINDOW.
    return SIM_PVREG_WINDOW % static_cast<size_t>(sim().pagesize) == 0;
}

// The sim console's device window (arch.h). The host models exactly one device register block,
// so that block is the only thing a sim console driver can ever be granted. Zero before
// arch_init has mapped a candidate, which reads as "no window".
//
// MUST STAY IN THIS TU, like arch_periph_reg_write below: sim.cc is always extracted and is the
// first member of kickos_arch_sim, so it resolves the symbol before
// common/arch_console_reclaim_window_default.cc could be pulled in, which is why that fallback is
// dropped from this archive (arch/CMakeLists.txt).
void arch_console_reclaim_window(uintptr_t* base, size_t* size)
{
    *base = reinterpret_cast<uintptr_t>(sim().pvreg);
    *size = 0;
    if (sim().pvreg != nullptr)
    {
        *size = SIM_PVREG_WINDOW;
    }
}

// Privileged single-register write (arch.h). The caller's possession of the block at `base` and
// its containment of `[base + offset, +4)` are already checked in the syscall layer; this decides
// whether that exact register is on the allowlist above and whether the value stays inside the
// entry's mask. The value is never trimmed and never read-modify-written.
//
// THIS DEFINITION MUST STAY IN THIS TU. sim.cc is always extracted and is the FIRST member of
// kickos_arch_sim, so it resolves the symbol before common/arch_periph_reg_write_default.cc
// could be pulled in, which is why that fallback is dropped from this archive
// (arch/CMakeLists.txt).
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

// Nothing but the CPU reads the arena here, and mprotect carries no memory type.
int arch_mpu_nocache_support(void)
{
    return ARCH_MPU_NOCACHE_ALREADY;
}

// Rule 7: the sim's "devices" are arena-backed fakes reached via a data grant, so it reserves
// nothing and only the arena and encodability rules apply.
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

// arch_domain_static_regions lives in kernel/domain/domain.cc. On this build the weak
// __kickos_code_*/__kickos_appdata_* linker symbols are undefined, so it returns 0: the app's
// code and data are host-process memory and the sim governs only the arena.

// GNU ld default-script symbols bounding the host executable image (ELF header .. end
// of .bss). Array form + a uintptr_t decay dodges -Warray-compare.
extern "C" unsigned char __executable_start[];
extern "C" unsigned char _end[];

// The confused-deputy floor's read hook (arch.h). A string literal or thread name the app hands
// the kernel lives in the host binary's image, not the mprotect'd arena, so no per-domain region
// can name it: admit a range wholly inside the image and clear of the arena. The image also holds
// the kernel's own code and data (one binary), so this cannot separate app rodata from kernel
// statics; the cross-domain arena boundary stays closed, an arena range being disjoint from the
// image.
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

// The write twin (arch.h). Needed even with KICKOS_HAVE_MPU=1: sim app globals live in the host
// image, not the arena, so arch_domain_static_regions models no static-data region here.
// Admission is the read hook's rule: wholly inside the host image, clear of the arena.
//
// The image-bound symbols cannot separate .text/.rodata from .data/.bss, so an unprivileged
// out-pointer aimed at kernel state is admitted with no backstop. The sim enforces only the
// arena cross-domain boundary.
bool arch_user_data_writable(uintptr_t ptr, size_t len)
{
    return arch_user_text_readable(ptr, len);
}

uintptr_t arch_mpu_probe_addr(void)
{
    return reinterpret_cast<uintptr_t>(sim().guard);
}

// --- Syscall trap -----------------------------------------------------------
// A direct call, with the privilege raise below emulated. This backend ships no
// ipc_fastpath.cmake: a caller's continuation here is a host return address on its own stack,
// not a saved register frame a reply could land in.
uint64_t arch_syscall64(uintptr_t nr,
                        uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    // Emulated privilege raise, tracked PER-CONTEXT via SimContext::raised so it survives a
    // blocking switch. On a resume mid-syscall guard_apply_current() re-raises, the switch-in's
    // arch_mpu_apply having reinstated the caller's resting posture while it is still running
    // kernel code. The final unwind drops back to sim().applied.
    SimContext* self = nullptr;
    if (sim().arena != nullptr and sim().current != nullptr)
    {
        self = sim().current;
        self->raised = self->raised + 1;
        arena_raise_all();
    }
    uint64_t r = syscall_dispatch(nr, a0, a1, a2, a3);
    if (self != nullptr)
    {
        self->raised = self->raised - 1;
        if (self->raised == 0)
        {
            arena_lower_to_applied();
        }
    }
    return r;
}

uintptr_t arch_syscall(uintptr_t nr,
                       uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    return static_cast<uintptr_t>(arch_syscall64(nr, a0, a1, a2, a3));
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
    sim().irq_masked = sim().irq_masked | static_cast<sig_atomic_t>(1u << line);
    arch_irq_restore(s);
}

void arch_irq_unmask(int line)
{
    if (line < 0 or line >= SIM_IRQ_LINES)
    {
        return;
    }
    arch_irq_state_t s = arch_irq_save();
    sim().irq_masked = sim().irq_masked & static_cast<sig_atomic_t>(~(1u << line));
    // Latch-and-coalesce: a raise taken while the line was masked redelivers now through the
    // ISR path. It pends under this bracket (SIGUSR1 blocked) and lands at its release, in ISR
    // context.
    if (static_cast<unsigned>(sim().irq_pending) & (1u << line))
    {
        sim().irq_pending = sim().irq_pending & static_cast<sig_atomic_t>(~(1u << line));
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
    sim().irq_pending = sim().irq_pending & static_cast<sig_atomic_t>(~(1u << line));
    arch_irq_restore(s);
}

void arch_irq_inject(int irq)
{
    arch_irq_state_t s = arch_irq_save();
    // Latch-and-coalesce: a raise on a masked in-range line sets the one-deep pending
    // bit (redelivered at unmask), NOT dropped. An unmasked line, or a never-maskable
    // line >= SIM_IRQ_LINES, delivers now (the raise pends under this bracket and lands
    // at its release, in ISR context).
    if (irq >= 0 and irq < SIM_IRQ_LINES
        and (static_cast<unsigned>(sim().irq_masked) & (1u << irq)))
    {
        sim().irq_pending = sim().irq_pending | static_cast<sig_atomic_t>(1u << irq);
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
