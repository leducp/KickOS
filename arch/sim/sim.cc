// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/arch/arch.h>

#include <kickos/arch/clk_q32.h>
#include <kickos/console_tx.h>
#include <kickos/instance_local.h>
#include <kickos/sys/atomic.h>

#include <fatal_status.ld.h>

#include <kickos/chip_limits.h>

#include "chip_layout.h"
#include "irq.h"
#include "sim_mpu.h"

#include <new>

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

#include <kickos/sys/errno.h>

// Pre-4.17 headers: the returned-address check at the mmap call site is the guard.
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0
#endif

#include <kickos/trace/record.h>
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
#include <kickos/rtt.h>
#endif

static_assert(KICKOS_TRACE_ARCH == kickos::trace::ARCH_SIM,
              "KICKOS_TRACE_ARCH does not match ArchId::ARCH_SIM for sim");

namespace kickos
{
    void kprintf(char const* fmt, ...) __attribute__((format(printf, 1, 2)));
#if KICKOS_MULTI_INSTANCE
    int kmain(int argc, char** argv);
#endif
}
extern "C" void kpanic_enter(void);
extern "C" void kfault_terminate(void);
extern "C" void kickos_terminal_outcome_apply(void);

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

#if KICKOS_MULTI_INSTANCE
    // Longest line held before it is emitted unterminated. A cap, not a limit on output:
    // a longer line is simply split across two writes.
    enum { SIM_OUT_LINE_MAX = 240 };
#endif

    // Several sim backends (one per emulated MCU / KickCAT slave) co-reside in one
    // host process, so this state is instance-scoped and never a plain global.
    struct SimInstance
    {
        // --- running-context tracking + deferred-switch state ---
        SimContext* current = nullptr;
        Atomic<sig_atomic_t, Order::RELAXED> isr_depth = 0;

        // --- signal set covering all "interrupt" sources (crit-section mask) ---
        sigset_t irq_signals;

        // --- tickless one-shot timer ---
        timer_t timer;
        bool timer_created = false;

        // --- MPU: page-granular user-RAM arena governed by the emulation ---
        long pagesize = 0;
        unsigned char* arena = nullptr;
        size_t arena_size = 0;
        size_t arena_used = 0;
        // SIM_PVREG_SPAN bytes outside the arena. Null: the host refused the fixed mapping, so no
        // DEV window is encodable and the privileged-write seam declines.
        unsigned char* pvreg = nullptr;

        // The running thread's resting region set, so the syscall raise lowers back
        // to exactly it. This is the caller's own TCB regions[], valid only while
        // that thread runs.
        struct arch_mpu_region const* applied = nullptr;
        size_t applied_n = 0;

        // --- emulated device IRQ hand-off (async-signal to ISR) ---
        Atomic<sig_atomic_t, Order::RELAXED> pending_irq = -1;
        // Bit L set: line L masked, a raise latches in irq_pending. Every line starts masked.
        Atomic<sig_atomic_t, Order::RELAXED> irq_masked = static_cast<sig_atomic_t>(0xFFFFFFFFu);
        // Bit L set: a raise landed on line L while masked, one deep, redelivered at unmask.
        Atomic<sig_atomic_t, Order::RELAXED> irq_pending = 0;

        // --- emulated buffered-console TX-empty interrupt source ---
        // tx_enabled is the peripheral-level enable, tx_asserted the level line. tx_budget is a
        // synthetic per-delivery slot budget: without it the ring would drain in one shot and
        // never fill or wrap. It bites only inside the drain ISR.
        Atomic<sig_atomic_t, Order::RELAXED> tx_enabled = 0;
        Atomic<sig_atomic_t, Order::RELAXED> tx_asserted = 0;
        Atomic<sig_atomic_t, Order::RELAXED> in_tx_isr = 0;
        int tx_budget = 0;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
        // The physically-outgoing tid is armed before each swap; whichever context resumes
        // consumes it and emits the SWITCH record.
        Atomic<sig_atomic_t, Order::RELAXED> switch_pending = 0;
        uint16_t switch_from = 0xFFFF;
#endif

#if KICKOS_MULTI_INSTANCE
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
    // Owed after every context resume, a new thread's first run included.
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
    // The first candidate that maps is published as sim().pvreg, the only base the seam answers
    // for. user/apps/common/selftest walks the same list in the same order.
    constexpr uintptr_t SIM_PVREG_BASES[] = {
        0x40000000u,          // 1 GiB
        0x100000000ull,       // 4 GiB
        0x400000000ull,       // 16 GiB
        0x10000000000ull,     // 1 TiB
        0x100000000000ull,    // 16 TiB
    };
    // Every host page size in practice (4/16/64 KiB) divides it, so mprotect can describe it.
    constexpr size_t SIM_PVREG_WINDOW = 0x10000u;
    // An allowlist entry sits OUTSIDE the first window with host pages still behind it, so a
    // store there is refused by the kernel's containment check, never faulted on. The third
    // window is the console's registers: a published console's task alone may hold it, which
    // leaves the first two to every other task.
    constexpr size_t SIM_PVREG_CONSOLE = 2u * SIM_PVREG_WINDOW;
    constexpr size_t SIM_PVREG_SPAN = 3u * SIM_PVREG_WINDOW;

    struct SimPrivWriteReg
    {
        uintptr_t offset; // from the published base
        uint32_t mask;    // the only bits this entry may set
    };

    // Withholds whole bytes at both ends of the word: widening the column shows up as a store
    // that lands.
    constexpr uint32_t PVREG_MASKED_GRANT = 0x0000C3FFu;
    // Written through the seam only by a holder of a window wider than SIM_PVREG_WINDOW,
    // which this backend never admits.
    constexpr uint32_t PVREG_BEYOND_GRANT = 0x00000001u;

    constexpr SimPrivWriteReg SIM_PRIV_WRITE_REGS[] = {
        { 0x010u, PVREG_MASKED_GRANT },
        { SIM_PVREG_WINDOW, PVREG_BEYOND_GRANT },
    };

    // An entry outside the mapping would fault inside the privileged store and end the system.
    static_assert(SIM_PRIV_WRITE_REGS[0].offset + sizeof(uint32_t) <= SIM_PVREG_SPAN
                      and SIM_PRIV_WRITE_REGS[1].offset + sizeof(uint32_t) <= SIM_PVREG_SPAN,
                  "a sim allowlist entry lies outside the mapped fake register block");
    static_assert(SIM_PRIV_WRITE_REGS[1].offset >= SIM_PVREG_WINDOW,
                  "the containment entry must sit beyond the grantable window");
    // A base the window does not divide would hand the app a misaligned DEV window.
    static_assert(SIM_PVREG_BASES[0] % SIM_PVREG_WINDOW == 0
                      and SIM_PVREG_BASES[1] % SIM_PVREG_WINDOW == 0
                      and SIM_PVREG_BASES[2] % SIM_PVREG_WINDOW == 0
                      and SIM_PVREG_BASES[3] % SIM_PVREG_WINDOW == 0
                      and SIM_PVREG_BASES[4] % SIM_PVREG_WINDOW == 0,
                  "every candidate base must be window-aligned");

    void arena_lower_to_applied();

    // makecontext takes ints, so the context pointer travels as two.
    void trampoline(unsigned hi, unsigned lo)
    {
        uintptr_t p = (static_cast<uintptr_t>(hi) << 32) | static_cast<uintptr_t>(lo);
        SimContext* c = reinterpret_cast<SimContext*>(p);
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
        // A new thread's first run resumes here, not after a swapcontext.
        trace_emit_switch_in();
#endif
        // arch_context_init blocked the IRQ signals; this is the matching unblock, and
        // it must precede the arena transition below, whose arch_irq_restore is exact
        // and would otherwise re-block them for this thread's whole life.
        sigprocmask(SIG_UNBLOCK, &sim().irq_signals, nullptr);
        // arch_mpu_apply at the starting switch left the arena raised: lower to this thread's
        // resting posture here, on its own stack, masked so no ISR reprograms the arena mid-way.
        {
            arch_irq_state_t s = arch_irq_save();
            arena_lower_to_applied();
            arch_irq_restore(s);
        }
        c->entry(c->arg);
        kickos_thread_return();
    }

    // Whole arena accessible, for kernel code. An unprivileged thread's stack is an arena
    // block, so kernel code running on it must keep it mapped.
    void arena_raise_all()
    {
        if (sim().arena != nullptr)
        {
            mprotect(sim().arena, sim().arena_size, PROT_READ | PROT_WRITE);
        }
    }
    // Fail closed: a hostile grant must never mprotect host memory or de-execute code.
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
    // Never transiently unmaps a granted region: the caller runs on its own arena-resident
    // stack, which would fault on the next push. PROT_NONE only the gaps, then set each region.
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
        kickos::sim::protect_in_slot_order(
            sim().applied, sim().applied_n, arena_region_valid,
            [](uintptr_t base, size_t size, int prot) {
                mprotect(reinterpret_cast<void*>(base), size, prot);
            });
    }

    // A context mid-syscall keeps whole-arena access across a blocking switch back into it.
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
            // Several wakes in one ISR collapse to this one swap, so one SWITCH record.
            trace_switch_arm(interrupted->tid);
#endif
            swapcontext(&interrupted->uc, &sim().current->uc);
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
            trace_emit_switch_in();
#endif
        }
    }

    // --- Buffered console TX backend (console_tx.h) ----------------------------
    enum
    {
        TX_LINE = kickos::sim::irq::CONSOLE_TX,
        TX_BUDGET = 8 // bytes drained per ISR delivery
    };

    int sim_tx_slot_free()
    {
        // The synchronous paths must never stall.
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

#if KICKOS_MULTI_INSTANCE
    // Process wide, not instance local: it states whether this stdout is shared. A lone instance
    // emits byte for byte what a build without the knob emits.
    bool g_out_tagged = false;

    // One write keeps a line intact against the co-residents sharing this fd. Async-signal-safe:
    // the tag is formed by hand, not by printf.
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
        // Cleared before the write: a fault inside it must not leave the line for the shutdown
        // flush to emit twice.
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
#if KICKOS_MULTI_INSTANCE
        sim_line_put(c);
#else
        while (true)
        {
            ssize_t w = write(1, &c, 1);
            if (w == 1)
            {
                break;
            }
            // A scheduling signal (no SA_RESTART) can interrupt the write.
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
        // Entered under IrqLock with SIGUSR1 blocked: the drain runs in ISR context once the
        // lock releases, never inline in the producer.
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
        sim_tx_slot_free, sim_tx_push, sim_tx_irq_enable, sim_tx_irq_disable,
        &kickos_console_tx_storage};

    // Re-asserts the level line while the IRQ stays enabled; console_tx_isr's irq_disable at an
    // empty ring ends the chain.
    void console_tx_service()
    {
        sim().tx_budget = TX_BUDGET;
        sim().in_tx_isr = 1;
        kickos_isr_irq(TX_LINE);
        sim().in_tx_isr = 0;
        if (sim().tx_enabled != 0)
        {
            sim().tx_asserted = 1;
            raise(SIGUSR1); // pends while SIGUSR1 is blocked in the handler, redelivered on return
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
        // The emulated TX-empty line shares this signal.
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

    // The seam is handed the ucontext alone, so the siginfo facts travel here: written just
    // before kickos_fault_kill_thread, read only inside it.
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
        // Before isr_frame_enter: arch_fault_is_user_thread reads isr_depth to tell a fault in
        // an ISR body from one in the thread it interrupted.
        sim_fault() = {addr, si->si_code, true};
        if (kickos_fault_kill_thread(ucontext))
        {
            return; // sigreturn resumes at kickos_thread_fault_exit
        }
        kickos_fault_report_begin();
        // The console routing must see arch_in_isr() to take the synchronous writer, or the fault
        // line sits in the buffered ring and is lost when this handler exits.
        isr_frame_enter();
        // A garbage context traps on an instruction fetch at 0 and reads like a null store in the
        // shared banner. SI_KERNEL with pc == 0 is the tell, which si_addr alone cannot show.
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
        // Read and write are not portably distinguishable here; reported as a write.
        kickos_isr_fault(addr, 1);
        // Returning would re-execute the faulting store: halt.
        arch_shutdown(2);
    }

    // Host x86 `ud2` from __builtin_trap. kpanic_enter forces the synchronous writer, so the dump
    // survives an armed console ring.
    void on_sigill(int, siginfo_t* si, void* ucontext)
    {
        // si_addr is the faulting PC here, not a data address.
        sim_fault() = {0, si->si_code, false};
        if (kickos_fault_kill_thread(ucontext))
        {
            return;
        }
        kickos_fault_report_begin();
        isr_frame_enter();
        kpanic_enter();
        ::kickos::kprintf("\n=== SIM FAULT (illegal instruction) at %p ===\n", si->si_addr);
        kfault_terminate();
    }

    // Only async-signal-safe calls.
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
    // on top of the domain-data allocs and the probe page.
    sim().arena_size = KICKOS_LAYOUT_ARENA_SIZE;
    sim().arena = static_cast<unsigned char*>(
        mmap(nullptr, sim().arena_size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (sim().arena == MAP_FAILED)
    {
        sim().arena = nullptr;
        sim().arena_size = 0;
    }
    sim().arena_used = 0;
    sim().applied_n = 0;

    // The fake register block lies outside the arena: it must not be reachable as a RAM grant.
    // Kept if already mapped: a second arch_init must not drop the block the published base and
    // every live DEV grant refer to.
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
#if KICKOS_MULTI_INSTANCE
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
    sa.sa_mask = sim().irq_signals;

    sa.sa_sigaction = on_sigalrm;
    sigaction(SIGALRM, &sa, nullptr);
    sa.sa_sigaction = on_sigusr1;
    sigaction(SIGUSR1, &sa, nullptr);

    struct sigaction fa{};
    fa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    // A deferred switch out of the fault handler would abandon its alt-stack frame mid-report.
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
#if KICKOS_MULTI_INSTANCE
    // SIGEV_SIGNAL is process-directed: one instance's tick could land on another's host thread
    // and the instance that armed it would never wake.
    sev.sigev_notify = SIGEV_THREAD_ID;
    // glibc has no name for the tid member.
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

// The one backend that does not switch stacks (arch.h): KICKOS_PANIC_STACK_SIZE is 0, a host
// stack is megabytes, and a hand-switched one would surprise the host's signal and coroutine
// machinery.
void kickos_panic_stack_enter(char const* msg, char const* file, unsigned line, uintptr_t top)
{
    (void)top;
    kickos_panic_report(msg, file, line);
}

// The blink fallback would spin forever: the sim must exit so CTest sees the status.
void kfault_terminate(void)
{
    arch_console_flush_sync();
    kickos_terminal_outcome_apply();
    arch_shutdown(KICKOS_FATAL_STATUS);
}

void arch_shutdown(int status)
{
    // Masked to _exit: arch_shutdown holds no lock, so a timer ISR could swap away from root
    // mid-drain and strand the ring, or emit records after the closing SESSION's snapshot.
    (void)arch_irq_save();
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Before the console drain: report_counters prints through the buffered ring.
    kickos_trace_final_session();
    kickos_trace_report_counters();
#endif
    // Anything still enqueued is stranded by _exit. Safe because the drain ISR cannot run masked.
    console_tx_flush_sync();
#if KICKOS_MULTI_INSTANCE
    sim_line_flush();
#endif
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // Path is $KICKOS_TRACE_FILE or a default in the CWD.
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
#if KICKOS_MULTI_INSTANCE
    // Ends this instance, not the process; setcontext returns only on failure.
    if (sim().exit_armed)
    {
        sim().exit_status = status;
        setcontext(&sim().exit_uc);
    }
#endif
    _exit(status);
}

#if KICKOS_MULTI_INSTANCE
// Settled before the first instance thread: every console flush reads it, a signal handler's
// included.
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
        // Resumed from arch_shutdown, which restored the capture-time signal mask: block this
        // instance's interrupts and drop its timer.
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

bool arch_console_write_sync(char const* buf, size_t n)
{
#if KICKOS_MULTI_INSTANCE
    for (size_t i = 0; i < n; i++)
    {
        sim_line_put(buf[i]);
    }
    // Panic and fault do not return to a newline, so an unterminated tail would be lost.
    sim_line_flush();
    return true;
#else
    size_t off = 0;
    while (off < n)
    {
        ssize_t w = write(1, buf + off, n - off);
        if (w < 0)
        {
            // A scheduling signal (no SA_RESTART) can interrupt the write.
            if (errno == EINTR)
            {
                continue;
            }
            return false;
        }
        if (w == 0)
        {
            return false;
        }
        off += static_cast<size_t>(w);
    }
    return true;
#endif
}

console_tx_backend const* arch_console_tx_backend(int* irq_line)
{
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
    // An MCU-sized caller stack would overflow the host: a host stack is substituted below
    // this floor and never freed.
    constexpr size_t SIM_HOST_MIN_STACK = 64 * 1024;
    if (stack_size < SIM_HOST_MIN_STACK)
    {
        stack_base = malloc(SIM_HOST_MIN_STACK);
        stack_size = SIM_HOST_MIN_STACK;
    }
    // Not memset: SimContext holds atomics.
    new (static_cast<void*>(c)) SimContext{};
    context_capture(&c->uc);
    // Not the creating thread's mask, which may be inside a critical section.
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
    c->uc.uc_stack.ss_sp = stack_base;
    c->uc.uc_stack.ss_size = stack_size;
    c->uc.uc_link = nullptr;
    c->entry = entry;
    c->arg = arg;
    // Privilege is the guard-page posture and the `raised` count, not stored here.
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
    // (1) Reuse the host stack this context runs on: re-deriving from the caller's fields would
    // malloc another substitute per rebuild.
    void* const host_base = c->uc.uc_stack.ss_sp;
    size_t const host_size = c->uc.uc_stack.ss_size;
    if (host_base != nullptr and host_size != 0)
    {
        stack_base = host_base;
        stack_size = host_size;
    }
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    // The trace id is stamped once at thread_create and arch_context_init clears it.
    uint16_t const tid = c->tid;
#endif
    arch_context_init(ctx, entry, nullptr, stack_base, stack_size, 1);
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    c->tid = tid;
#endif
    // (2) Raise: switch-in programs the thread's resting grant, so the stub would SIGSEGV on its
    // first read of kernel state. The count holds the arena up across the teardown's blocking
    // points and is never unwound, the stub never returning. Not the running context: its
    // switch-in raises, so do not call arena_raise_all() here.
    c->raised = c->raised + 1;
}

void arch_switch(struct arch_context* from, struct arch_context* to)
{
    SimContext* t = sc(to);
    if (sim().isr_depth > 0)
    {
        // The swap waits for interrupt exit.
        sim().current = t;
        guard_apply_current();
        return;
    }
    SimContext* f = sc(from);
    sim().current = t;
    guard_apply_current();
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    trace_switch_arm(f->tid);
#endif
    swapcontext(&f->uc, &t->uc);
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    trace_emit_switch_in();
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
    // The first switch has no outgoing thread.
    trace_switch_arm(static_cast<uint16_t>(kickos::trace::TRACE_NO_THREAD));
#endif
    swapcontext(&b->uc, &f->uc);
#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    trace_emit_switch_in();
#endif
}

// --- Critical section -------------------------------------------------------
arch_irq_state_t arch_irq_save(void)
{
    sigset_t prev;
    sigprocmask(SIG_BLOCK, &sim().irq_signals, &prev);
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

// Microseconds as a u32, wrapping at ~71 min.
uint32_t arch_trace_now(void)
{
    return static_cast<uint32_t>(arch_clock_now() / 1000ull);
}

uint64_t arch_cpu_clock_hz(void)
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
// Runs at switch-in for the INCOMING thread while still executing on the OUTGOING thread's
// stack, arch_switch not having swapped yet. THIS MUST NOT LOWER TO THE INCOMING RESTING
// POSTURE: that would PROT_NONE the outgoing thread's arena-resident stack and fault the
// swap itself. It RECORDS the incoming set and RAISES the arena, and the incoming
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
// resolve, arch.h requiring a definition of it from every backend.
void kickos_arch_mpu_commit(void) {}

// Nothing is deferred on this backend, so the set is already live when apply returns.
void arch_mpu_apply_now(struct arch_mpu_region const* regions, size_t n,
                        struct arch_mpu_encoded const* image)
{
    arch_mpu_apply(regions, n, image);
}

// --- Fault isolation --------------------------------------------------------
// arch_syscall raises the WHOLE arena for the duration of dispatch and tracks it per-context in
// SimContext::raised, so `raised == 0` at fault time carries what CONTROL.nPRIV carries on ARM:
// user code, not kernel code the thread merely called. isr_depth covers a fault inside an ISR.
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
    // The stub is kernel code reaching outside the resting grant: raise as arch_syscall does,
    // never unwound, the stub never returning.
    if (sim().current != nullptr)
    {
        sim().current->raised = sim().current->raised + 1;
        arena_raise_all();
    }
    uc->uc_mcontext.gregs[REG_RIP] =
        static_cast<greg_t>(reinterpret_cast<uintptr_t>(&kickos_thread_fault_exit));
    // Guarded on the faulting RSP lying inside the recorded stack: a substituted host stack is
    // not what those fields describe. System V AMD64 wants rsp+8 16-byte aligned at the
    // callee's first instruction, and this jump skips the call that pushes the return address,
    // so the slot is reserved and filled with one that cannot be taken.
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
    return static_cast<size_t>(sim().pagesize);
}

// mprotect governs only the arena, so the one admitted window is the fake register block at
// its published base; everything else fails closed.
bool arch_mpu_region_encodable(uintptr_t base, size_t size)
{
    if (sim().pvreg == nullptr or sim().pagesize <= 0)
    {
        return false;
    }
    // One window of the span, exactly, never a sub-range nor two at once: the allowlist entry
    // at SIM_PVREG_WINDOW is refused by containment only while no wider window is grantable.
    uintptr_t const pv = reinterpret_cast<uintptr_t>(sim().pvreg);
    if ((base != pv and base != pv + SIM_PVREG_WINDOW and base != pv + SIM_PVREG_CONSOLE)
        or size != SIM_PVREG_WINDOW)
    {
        return false;
    }
    // A host page coarser than the window leaves it undescribable by mprotect.
    return SIM_PVREG_WINDOW % static_cast<size_t>(sim().pagesize) == 0;
}

// The third window of the fake register block. Zero before arch_init maps one.
//
// MUST STAY IN THIS TU, with arch_periph_reg_write below: sim.cc is the first member of
// kickos_arch_sim and always extracted, which is why arch/sim/sources.cmake drops both fallbacks.
void arch_console_reclaim_window(uintptr_t* base, size_t* size)
{
    *base = 0;
    *size = 0;
    if (sim().pvreg != nullptr)
    {
        *base = reinterpret_cast<uintptr_t>(sim().pvreg) + SIM_PVREG_CONSOLE;
        *size = SIM_PVREG_WINDOW;
    }
}

// The syscall layer has checked possession of the block and containment of `[base + offset,
// +4)`; this decides allowlist membership and the entry's mask. The value is never trimmed.
int arch_periph_reg_write(uintptr_t base, uintptr_t offset, uint32_t value)
{
    if (sim().pvreg == nullptr)
    {
        return -KOS_ENOSYS;
    }
    if (base != reinterpret_cast<uintptr_t>(sim().pvreg))
    {
        return -KOS_EINVAL;
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

int arch_mpu_region_pow2(void)
{
    return 0;
}

// Nothing but the CPU reads the arena.
int arch_mpu_nocache_support(void)
{
    return ARCH_MPU_NOCACHE_ALREADY;
}

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
    size_t const rsz = arch_ram_region_size(size);
    size_t const ralign = arch_ram_region_align(size);
    uintptr_t const base = reinterpret_cast<uintptr_t>(sim().arena);
    uintptr_t const cur = base + sim().arena_used;
    // The subtract-form bounds are immune to the size_t wrap that (used + rsz) has.
    uintptr_t const aligned = (cur + (ralign - 1)) & ~static_cast<uintptr_t>(ralign - 1);
    size_t const off = static_cast<size_t>(aligned - base);
    if (aligned < cur or off > sim().arena_size or rsz > sim().arena_size - off)
    {
        return nullptr;
    }
    sim().arena_used = off + rsz;
    return reinterpret_cast<void*>(aligned);
}

// arch_domain_static_regions (kernel/domain/domain.cc) returns 0 here: start.cc states the app
// code and data windows empty, the app being host-process memory outside the arena.

// GNU ld default-script symbols bounding the host image. Arrays, to dodge -Warray-compare.
extern "C" unsigned char __executable_start[];
extern "C" unsigned char _end[];

// A string literal or thread name the app hands the kernel lives in the host image, not the
// arena, so no region can name it: admit a range wholly inside the image and clear of the arena.
// The image also holds the kernel's own statics, which this cannot separate from app rodata.
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

// The read hook's rule. An unprivileged out-pointer aimed at kernel state is admitted with no
// backstop: the sim enforces only the arena cross-domain boundary.
bool arch_user_data_writable(uintptr_t ptr, size_t len)
{
    return arch_user_text_readable(ptr, len);
}

// --- Syscall trap -----------------------------------------------------------
// A direct call. No ARCH_HAS_IPC_FASTPATH: a caller's continuation is a host return address on
// its own stack, not a saved register frame a reply could land in.
uint64_t arch_syscall64(uintptr_t nr,
                        uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    // Tracked per context in SimContext::raised: a switch-in reinstates the caller's resting
    // posture while it still runs kernel code, and guard_apply_current re-raises.
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
// Width of the irq_masked bitset: a line at or above it is never maskable.
enum
{
    SIM_IRQ_LINES = 32
};
static_assert(SIM_IRQ_LINES == KICKOS_MAX_IRQ, "the sim's line count is its chip file's");

// Self-bracketed: a bare read-modify-write preempted mid-update would write back a stale mask
// and re-enable a mid-service line.
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
    // The raise pends under this bracket and lands at its release, in ISR context.
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

bool arch_irq_inject(int irq)
{
    if (irq < 0 or irq >= SIM_IRQ_LINES)
    {
        return false;
    }
    arch_irq_state_t s = arch_irq_save();
    // A masked line latches the one-deep pending bit; an unmasked one delivers when this
    // bracket releases.
    if ((static_cast<unsigned>(sim().irq_masked) & (1u << irq)) != 0u)
    {
        sim().irq_pending = sim().irq_pending | static_cast<sig_atomic_t>(1u << irq);
    }
    else
    {
        sim().pending_irq = irq;
        raise(SIGUSR1);
    }
    arch_irq_restore(s);
    return true;
}

// --- Idle -------------------------------------------------------------------
void arch_idle_wait(void)
{
    sigset_t empty;
    sigemptyset(&empty);
    sigsuspend(&empty); // atomically unblock + wait for a signal
}
}
