// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KickOS sim arch backend (host x86-64 Linux). The ONLY place host libc is
// used. Design studied from the RIOT `native` port: ucontext-based context
// switch, signal-driven timer + emulated device IRQs, mprotect over an mmap'd
// guard page for MPU emulation. Deferred context switches run on signal exit,
// mirroring the ARM PendSV-on-exception-return model.

#include <kickos/arch/arch.h>

#include <ucontext.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

namespace
{

    // --- Internal context layout over the opaque arch_context storage ----------
    struct SimContext
    {
        ucontext_t uc;
        void (*entry)(void*);
        void* arg;
        volatile int raised; // >0 while mid-syscall (read from signal-driven switch)
    };
    static_assert(sizeof(SimContext) <= ARCH_CONTEXT_SIZE,
                  "SimContext exceeds ARCH_CONTEXT_SIZE; grow arch/sim context.h");

    inline SimContext* sc(struct arch_context* c)
    {
        return reinterpret_cast<SimContext*>(c->opaque);
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

        // The running thread's resting region set, remembered so the syscall raise
        // can be lowered back to exactly it. We keep the caller's pointer (its TCB
        // regions[], stable while it runs) rather than a fixed-size copy -- no cap,
        // no truncation regardless of KICKOS_MPU_MAX_REGIONS.
        struct arch_mpu_region const* applied = nullptr;
        size_t applied_n = 0;

        // --- emulated device IRQ hand-off (async-signal to ISR) ---
        volatile sig_atomic_t pending_irq = -1;
        volatile sig_atomic_t irq_masked = 0; // bit L set => line L suppressed
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

    // --- Trampoline pointer packing (makecontext takes ints) -------------------
    void trampoline(unsigned hi, unsigned lo)
    {
        uintptr_t p = (static_cast<uintptr_t>(hi) << 32) | static_cast<uintptr_t>(lo);
        SimContext* c = reinterpret_cast<SimContext*>(p);
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

    void arena_protect_none()
    {
        if (sim().arena != nullptr)
        {
            mprotect(sim().arena, sim().arena_size, PROT_NONE);
        }
    }
    // Privileged posture: whole arena accessible (the background-region analog).
    void arena_raise_all()
    {
        if (sim().arena != nullptr)
        {
            mprotect(sim().arena, sim().arena_size, PROT_READ | PROT_WRITE);
        }
    }
    // Grant a validated region set: each region must be a page-aligned sub-range
    // of the arena, else it is skipped (fail-closed) so a bad/hostile grant can
    // never mprotect host memory or de-execute code.
    void grant_region_set(struct arch_mpu_region const* regions, size_t n)
    {
        uintptr_t astart = reinterpret_cast<uintptr_t>(sim().arena);
        size_t pg = static_cast<size_t>(sim().pagesize);
        for (size_t i = 0; i < n; i++)
        {
            uintptr_t base = regions[i].base;
            size_t size = regions[i].size;
            if (size == 0 or size > sim().arena_size)
            {
                continue;
            }
            if (base < astart or base - astart > sim().arena_size - size)
            {
                continue;
            }
            if ((base - astart) % pg != 0 or size % pg != 0)
            {
                continue;
            }
            mprotect(reinterpret_cast<void*>(base), size, prot_from_attr(regions[i].attr));
        }
    }
    // Re-grant the remembered resting set (arena no-access, then each region).
    void arena_lower_to_applied()
    {
        arena_protect_none();
        grant_region_set(sim().applied, sim().applied_n);
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
            swapcontext(&interrupted->uc, &sim().current->uc);
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
        // We cannot distinguish read vs write portably here; report as write since
        // the demo/wild-write path is a store. Reporting routes through the kernel.
        kickos_isr_fault(addr, 1);
        // kickos_isr_fault is expected to terminate or recover; if it returns we
        // cannot safely resume the faulting store, so halt.
        arch_shutdown(2);
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

    // The user-RAM arena the MPU emulation governs (domain data + probe page).
    sim().arena_size = 256 * 1024;
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

void arch_shutdown(int status)
{
    _exit(status);
}

void arch_console_write(char const* buf, size_t n)
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

// --- Context / switching ---------------------------------------------------
void arch_context_init(struct arch_context* ctx,
                       void (*entry)(void*), void* arg,
                       void* stack_base, size_t stack_size,
                       int privileged)
{
    SimContext* c = sc(ctx);
    memset(c, 0, sizeof(*c));
    getcontext(&c->uc);
    // New threads always start with all interrupts enabled, independent of the
    // creating thread's current mask (which may be inside a critical section).
    sigemptyset(&c->uc.uc_sigmask);
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
    swapcontext(&f->uc, &t->uc);
}

void arch_start(struct arch_context* boot, struct arch_context* first)
{
    SimContext* b = sc(boot);
    SimContext* f = sc(first);
    memset(b, 0, sizeof(*b));
    getcontext(&b->uc);
    sim().current = f;
    swapcontext(&b->uc, &f->uc);
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
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(ts.tv_nsec);
}

void arch_timer_arm(uint64_t deadline_ns)
{
    if (not sim().timer_created)
    {
        return;
    }
    struct itimerspec its{};
    its.it_value.tv_sec = static_cast<time_t>(deadline_ns / 1000000000ull);
    its.it_value.tv_nsec = static_cast<long>(deadline_ns % 1000000000ull);
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
void arch_mpu_apply(struct arch_mpu_region const* regions, size_t n)
{
    if (sim().arena == nullptr)
    {
        return;
    }
    // Replace the whole active set: arena no-access, then grant this thread's
    // (validated) regions, and remember the set as its resting posture so a
    // syscall raise can be lowered back to exactly it. The regions pointer is
    // the caller's TCB regions[] -- stable while the thread runs.
    arena_protect_none();
    grant_region_set(regions, n);
    sim().applied = regions;
    sim().applied_n = n;
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
    if (sim().arena == nullptr)
    {
        return nullptr;
    }
    size_t pg = static_cast<size_t>(sim().pagesize);
    size_t need = (size + pg - 1) & ~(pg - 1);
    // Subtract-form bound is immune to the size_t wrap that (used + need) has.
    if (need == 0 or need > sim().arena_size - sim().arena_used)
    {
        return nullptr;
    }
    void* p = sim().arena + sim().arena_used;
    sim().arena_used += need;
    return p;
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
    kSimIrqLines = 32
};

void arch_irq_mask(int line)
{
    if (line < 0 or line >= kSimIrqLines)
    {
        return;
    }
    sim().irq_masked |= static_cast<int>(1u << line);
}

void arch_irq_unmask(int line)
{
    if (line < 0 or line >= kSimIrqLines)
    {
        return;
    }
    sim().irq_masked &= static_cast<int>(~(1u << line));
}

void arch_irq_inject(int irq)
{
    // A masked line's raise is dropped (level-coalesced): the driver re-arms by
    // unmasking at irq_ack, and the next raise delivers.
    if (irq >= 0 and irq < kSimIrqLines and (static_cast<unsigned>(sim().irq_masked) & (1u << irq)))
    {
        return;
    }
    sim().pending_irq = irq;
    raise(SIGUSR1); // delivered synchronously on this thread -> ISR context
}

// --- Idle -------------------------------------------------------------------
void arch_idle_wait(void)
{
    sigset_t empty;
    sigemptyset(&empty);
    sigsuspend(&empty); // atomically unblock + wait for a signal
}
}
