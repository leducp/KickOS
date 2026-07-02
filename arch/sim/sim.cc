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
        int privileged;
    };
    static_assert(sizeof(SimContext) <= ARCH_CONTEXT_SIZE,
                  "SimContext exceeds ARCH_CONTEXT_SIZE; grow arch/sim context.h");

    inline SimContext* sc(struct arch_context* c)
    {
        return reinterpret_cast<SimContext*>(c->opaque);
    }

    // --- Running-context tracking + deferred-switch state ----------------------
    SimContext* g_current = nullptr; // arch's view of the running ctx
    volatile sig_atomic_t g_isr_depth = 0;

    // --- Signal set covering all "interrupt" sources (crit-section mask) -------
    sigset_t g_irq_signals;

    // --- Tickless one-shot timer -----------------------------------------------
    timer_t g_timer;
    bool g_timer_created = false;

    // --- MPU guard page ---------------------------------------------------------
    long g_pagesize = 0;
    unsigned char* g_guard = nullptr;
    int g_guard_prot = PROT_READ | PROT_WRITE; // last resting protection

    // --- Emulated device IRQ hand-off (async-signal to ISR) --------------------
    volatile sig_atomic_t g_pending_irq = -1;

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
        if (attr & ARCH_MPU_R) prot |= PROT_READ;
        if (attr & ARCH_MPU_W) prot |= PROT_WRITE;
        if (attr & ARCH_MPU_X) prot |= PROT_EXEC;
        return prot;
    }

    // Run an ISR body with correct interrupt-depth bookkeeping and, on the way
    // out at depth 0, perform any deferred context switch requested during it.
    void isr_frame_enter() { g_isr_depth++; }
    void isr_frame_leave(SimContext* interrupted)
    {
        g_isr_depth--;
        if (g_isr_depth == 0 && g_current != interrupted)
        {
            swapcontext(&interrupted->uc, &g_current->uc);
        }
    }

    void on_sigalrm(int, siginfo_t*, void*)
    {
        SimContext* interrupted = g_current;
        isr_frame_enter();
        kickos_isr_timer();
        isr_frame_leave(interrupted);
    }

    void on_sigusr1(int, siginfo_t*, void*)
    {
        SimContext* interrupted = g_current;
        isr_frame_enter();
        int irq = g_pending_irq;
        g_pending_irq = -1;
        if (irq >= 0) kickos_isr_irq(irq);
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

}

// ===========================================================================
extern "C"
{

void arch_init(void)
{
    g_pagesize = sysconf(_SC_PAGESIZE);

    g_guard = static_cast<unsigned char*>(
        mmap(nullptr, g_pagesize, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (g_guard == MAP_FAILED) g_guard = nullptr; // MAP_FAILED is (void*)-1
    g_guard_prot = PROT_READ | PROT_WRITE;

    sigemptyset(&g_irq_signals);
    sigaddset(&g_irq_signals, SIGALRM);
    sigaddset(&g_irq_signals, SIGUSR1);

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
    sa.sa_mask = g_irq_signals; // IRQs don't nest each other

    sa.sa_sigaction = on_sigalrm;
    sigaction(SIGALRM, &sa, nullptr);
    sa.sa_sigaction = on_sigusr1;
    sigaction(SIGUSR1, &sa, nullptr);

    struct sigaction fa{};
    fa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    // Block timer/device IRQs while reporting a fault: a deferred switch out of
    // the fault handler would abandon its alt-stack frame mid-report.
    fa.sa_mask = g_irq_signals;
    fa.sa_sigaction = on_sigsegv;
    sigaction(SIGSEGV, &fa, nullptr);

    struct sigevent sev{};
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGALRM;
    if (timer_create(CLOCK_MONOTONIC, &sev, &g_timer) == 0)
    {
        g_timer_created = true;
    }
}

void arch_shutdown(int status)
{
    _exit(status);
}

void arch_console_write(char const* buf, size_t n)
{
    ssize_t off = 0;
    while (static_cast<size_t>(off) < n)
    {
        ssize_t w = write(1, buf + off, n - static_cast<size_t>(off));
        if (w <= 0) break;
        off += w;
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
    c->privileged = privileged;

    uintptr_t p = reinterpret_cast<uintptr_t>(c);
    unsigned hi = static_cast<unsigned>(p >> 32);
    unsigned lo = static_cast<unsigned>(p & 0xffffffffu);
    makecontext(&c->uc, reinterpret_cast<void (*)()>(trampoline), 2, hi, lo);
}

void arch_switch(struct arch_context* from, struct arch_context* to)
{
    SimContext* t = sc(to);
    if (g_isr_depth > 0)
    {
        // Defer the physical swap to interrupt exit (PendSV analogue).
        g_current = t;
        return;
    }
    SimContext* f = sc(from);
    g_current = t;
    swapcontext(&f->uc, &t->uc);
}

void arch_start(struct arch_context* boot, struct arch_context* first)
{
    SimContext* b = sc(boot);
    SimContext* f = sc(first);
    memset(b, 0, sizeof(*b));
    getcontext(&b->uc);
    g_current = f;
    swapcontext(&b->uc, &f->uc);
}

// --- Critical section -------------------------------------------------------
arch_irq_state_t arch_irq_save(void)
{
    sigset_t prev;
    sigprocmask(SIG_BLOCK, &g_irq_signals, &prev);
    // Encode whether SIGALRM was previously unblocked so restore is exact.
    arch_irq_state_t s = 0;
    if (!sigismember(&prev, SIGALRM)) s |= 1;
    if (!sigismember(&prev, SIGUSR1)) s |= 2;
    return s;
}

void arch_irq_restore(arch_irq_state_t state)
{
    sigset_t unblock;
    sigemptyset(&unblock);
    if (state & 1) sigaddset(&unblock, SIGALRM);
    if (state & 2) sigaddset(&unblock, SIGUSR1);
    if (state) sigprocmask(SIG_UNBLOCK, &unblock, nullptr);
}

int arch_in_isr(void) { return g_isr_depth > 0; }

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
    if (!g_timer_created) return;
    struct itimerspec its{};
    its.it_value.tv_sec = static_cast<time_t>(deadline_ns / 1000000000ull);
    its.it_value.tv_nsec = static_cast<long>(deadline_ns % 1000000000ull);
    // it_interval left zero -> one-shot.
    timer_settime(g_timer, TIMER_ABSTIME, &its, nullptr);
}

void arch_timer_disarm(void)
{
    if (!g_timer_created) return;
    struct itimerspec its{}; // all-zero disarms
    timer_settime(g_timer, 0, &its, nullptr);
}

// --- MPU (guard-page emulation) --------------------------------------------
void arch_mpu_apply(const struct arch_mpu_region* regions, size_t n)
{
    if (!g_guard) return;
    uintptr_t guard = reinterpret_cast<uintptr_t>(g_guard);
    int prot = PROT_NONE;
    for (size_t i = 0; i < n; i++)
    {
        uintptr_t base = regions[i].base;
        uintptr_t end = base + regions[i].size;
        if (guard >= base && guard < end) prot |= prot_from_attr(regions[i].attr);
    }
    g_guard_prot = prot; // the running thread's resting protection
    mprotect(g_guard, g_pagesize, prot);
}

uintptr_t arch_mpu_probe_addr(void)
{
    return reinterpret_cast<uintptr_t>(g_guard);
}

// --- Syscall trap -----------------------------------------------------------
uintptr_t arch_syscall(uintptr_t nr,
                       uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    // Emulated privilege raise: kernel/guard memory becomes accessible for the
    // duration of the dispatch, then drops back to the CALLER'S resting posture
    // (g_guard_prot, kept in sync by arch_mpu_apply on every switch-in) -- not an
    // unconditional PROT_NONE, which would corrupt a privileged caller's regions.
    int had_guard = 0;
    if (g_guard)
    {
        had_guard = 1;
        mprotect(g_guard, g_pagesize, PROT_READ | PROT_WRITE);
    }
    uintptr_t r = syscall_dispatch(nr, a0, a1, a2, a3);
    if (had_guard) mprotect(g_guard, g_pagesize, g_guard_prot);
    return r;
}

// --- Emulated device interrupt ---------------------------------------------
void arch_irq_inject(int irq)
{
    g_pending_irq = irq;
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
