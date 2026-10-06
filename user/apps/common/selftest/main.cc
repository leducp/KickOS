// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Unprivileged userspace tests with TAP output. Ordering checks use a semaphore-protected event
// log, not console order. The registration list stays in this file: its TAP_ADD redefinitions
// cut it into six regions, which a small board builds as separate images.

#include "selftest.h"

#include <kickos/arch/arch.h> // arch_syscall
#include <kickos/config/priorities.h> // KICKOS_PRIO_MIN
#include <kickos/sys/driver_service.h> // kickos::driver::trap
#include <kickos/sys/emit.h> // kickos::kconsole_write_all, kickos::stdout_write

#if KICKOS_LIBC_REENT
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#endif

using namespace selftest;

namespace
{


    char g_log[128];
    int g_logn = 0;

    void log_reset()
    {
        g_logn = 0;
        g_log[0] = 0;
    }

    // Worker threads only: CH_LOCK is a child-table index, meaningless in main.
    void log_put(char c)
    {
        kos_sem_wait(CH_LOCK);
        if (g_logn < static_cast<int>(sizeof(g_log)) - 1)
        {
            g_log[g_logn++] = c;
            g_log[g_logn] = 0;
        }
        kos_sem_post(CH_LOCK);
    }

    bool log_eq(char const* s)
    {
        return strlen(s) == static_cast<size_t>(g_logn) and memcmp(g_log, s, g_logn) == 0;
    }

    int count(char c)
    {
        int n = 0;
        for (int i = 0; i < g_logn; i++)
        {
            if (g_log[i] == c)
            {
                n++;
            }
        }
        return n;
    }

    // Index of the k-th (1-based) occurrence of c, or a large sentinel so that a
    // "not found" makes any `<` ordering assertion fail.
    int nth(char c, int k)
    {
        int seen = 0;
        for (int i = 0; i < g_logn; i++)
        {
            if (g_log[i] == c)
            {
                seen++;
                if (seen == k)
                {
                    return i;
                }
            }
        }
        return 1 << 30;
    }


    // --- A staging window, and reading back whether it held --------------------
    // Several arms below are instruments only while one span is still running when another
    // event falls due, and nothing the kernel publishes says whether it was. The spinning
    // thread stamps both ends of its span; the party it holds off stamps its due instant from
    // a clock read taken BEFORE its sleep, since a thread kept off the CPU cannot read the
    // clock at its own wake.
    //
    // A due instant is computed, not observed: a window reported held by less than the
    // read-to-syscall gap is not proof that it held.
    //
    // Stamps are the low 32 bits of kos_clock_now(): every span here is tens of milliseconds
    // inside a 4.29 s wrap, so the unsigned differences are unambiguous. Zero is "not
    // stamped", which costs one clock value in 2^32.
    struct Window
    {
        Atomic<uint32_t, Order::RELAXED> open{0};
        Atomic<uint32_t, Order::RELAXED> close{0};
    };

    uint32_t stamp_now()
    {
        return static_cast<uint32_t>(kos_clock_now());
    }

    // For a thread about to sleep: when it will be due, as it can state it.
    uint32_t stamp_due(uint64_t sleep_ns)
    {
        return static_cast<uint32_t>(kos_clock_now() + sleep_ns);
    }

    // Earlier, over stamps whose true separation is far below the wrap.
    bool stamp_before(uint32_t a, uint32_t b)
    {
        return (a - b) >= 0x80000000u;
    }

    void window_reset(Window& w)
    {
        w.open = 0;
        w.close = 0;
    }

    void window_open(Window& w)
    {
        w.close = 0;
        w.open = stamp_now();
    }

    void window_close(Window& w)
    {
        w.close = stamp_now();
    }

    bool window_held(Window const& w, uint32_t at)
    {
        uint32_t const open = w.open.load();
        uint32_t const close = w.close.load();
        if (open == 0 or close == 0 or at == 0)
        {
            return false;
        }
        return (at - open) < (close - open);
    }

    // Whether `at` fell before the span closed, however early. For a party that only has to
    // be READY while the span runs: falling due before it opened still leaves it waiting.
    bool window_reached(Window const& w, uint32_t at)
    {
        uint32_t const open = w.open.load();
        uint32_t const close = w.close.load();
        if (open == 0 or close == 0 or at == 0)
        {
            return false;
        }
        return stamp_before(at, close);
    }

#define WINDOW_UNSTAMPED ": the span was never stamped end to end"
#define WINDOW_BEFORE ": due %u us BEFORE a %u us span"
#define WINDOW_AFTER ": due %u us AFTER a %u us span"
    // `what` names the precondition in the arm's own terms, never a bare "conditions not met".
    // tap::skip_vacuous and not tap::skip: the gate permits this category whatever the name
    // and expects none of it, where an ordinary skip must be declared per board.
    void skip_window_lost_at(char const* what, Window const& w, uint32_t at)
    {
        uint32_t const open = w.open.load();
        uint32_t const close = w.close.load();
        if (open == 0 or close == 0 or at == 0)
        {
            tap::skip_vacuous("%s" WINDOW_UNSTAMPED, what);
            return;
        }
        unsigned const span_us = static_cast<unsigned>((close - open) / 1000u);
        if (stamp_before(at, open))
        {
            tap::skip_vacuous("%s" WINDOW_BEFORE, what,
                              static_cast<unsigned>((open - at) / 1000u), span_us);
            return;
        }
        tap::skip_vacuous("%s" WINDOW_AFTER, what,
                          static_cast<unsigned>((at - close) / 1000u), span_us);
    }

    template <size_t N>
    void skip_window_lost(char const (&what)[N], Window const& w, uint32_t at)
    {
        constexpr size_t tail = tap::format_chars_max(WINDOW_BEFORE);
        static_assert(tap::format_chars_max(WINDOW_AFTER) <= tail
                      and tap::format_chars_max(WINDOW_UNSTAMPED) <= tail);
        static_assert(N - 1 + tail <= tap::REASON_CHARS_MAX,
                      "this precondition can overrun the TAP line: shorten it");
        skip_window_lost_at(what, w, at);
    }
#undef WINDOW_UNSTAMPED
#undef WINDOW_BEFORE
#undef WINDOW_AFTER

    // Call ONLY after a failure: between every arm it churns a cap slot 90 times over and
    // starves the later spawns on the smallest board.
    void done_reset()
    {
        kos_sem_destroy(g_done);
        kos_sem_create(0, &g_done);
    }

    // Staging gate: every worker of a test must exist before ANY of them runs. It MUST be
    // its own semaphore: gating on the event-log mutex hands the token straight to the next
    // waiter and the workers ping-pong through log_put instead. stage_wait is the worker's
    // first statement; main posts once after the last spawn and each worker re-posts.
    kos_cap_t g_gate = KOS_CAP_NONE;
    void stage_release()
    {
        kos_sem_post(g_gate);
    }
    void stage_wait(kos_cap_t gate)
    {
        kos_sem_wait(gate);
        kos_sem_post(gate);
    }

    char arg_char(void* arg)
    {
        return static_cast<char>(reinterpret_cast<uintptr_t>(arg));
    }


    // A short count, or -KOS_EAGAIN, leaves the rest of s to the caller, and -KOS_EBUSY all of
    // it to this thread's stdout; any other refusal leaves nothing to send.
    void write_rest(char const* s, size_t n, int32_t took)
    {
        size_t sent = 0;
        if (took > 0)
        {
            sent = static_cast<size_t>(took);
        }
        else if (took == -KOS_EBUSY)
        {
            kickos::stdout_write(s, n);
            return;
        }
        else if (took != -KOS_EAGAIN)
        {
            return;
        }
        if (sent < n)
        {
            kickos::kconsole_write_all(s + sent, n - sent);
        }
    }

    // A count the console legally answers for a line of n bytes shorter than one syscall
    // chunk: a published console hands a served thread's line back to its stdout, a
    // partition's claim can end inside it, and anything else takes it whole. A hand-back is
    // checked against the endpoint, whose zero-length send is taken only where it serves.
    bool console_count_ok(int32_t took, size_t n)
    {
        if (took == -KOS_EBUSY)
        {
#if KICKOS_CONSOLE_CHIP
            return kos_send(KOS_CAP_STDOUT, "", 0) >= 0;
#else
            return false;
#endif
        }
#if KICKOS_AMP_OWN_IMAGE && KICKOS_CONSOLE_CHIP
        return took == -KOS_EAGAIN or (took > 0 and static_cast<size_t>(took) <= n);
#else
        return took == static_cast<int32_t>(n);
#endif
    }

    // --- SVC argument/return roundtrip -----------------------------------------
    // kos_kconsole_write returns `len` even when console_emit discards every byte of a thread
    // no published driver serves (kernel/init/console.cc, USER_OWNED), so this proves nothing
    // about delivery.
    void t_svc()
    {
        char const* s = "# [svc] kconsole_write arg/return roundtrip (not a delivery check)\n";
        size_t const n = strlen(s);
        int32_t const took = kos_kconsole_write(s, n);
        TAP_CHECK(console_count_ok(took, n));
        write_rest(s, n, took);
        TAP_CHECK(kos_kconsole_write(s, 0) == 0); // a len-0 write is a legitimate 0 (sys.h)
        // The prefix must itself be a whole line or the TAP stream is malformed.
        char const* pfx = "# [svc] len-honoured prefix\nTRAILING-MUST-NOT-APPEAR";
        size_t const cut = strlen("# [svc] len-honoured prefix\n");
        int32_t const cut_took = kos_kconsole_write(pfx, cut);
        TAP_CHECK(console_count_ok(cut_took, cut));
        write_rest(pfx, cut, cut_took);
    }

    // --- FIFO ordering ---------------------------------------------------------
    void fifo_worker(void* arg)
    {
        log_put(arg_char(arg));
        kos_sem_post(CH_DONE);
    }
    void t_fifo()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        log_reset();
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto a = kos::thread::create_caps(fifo_worker, reinterpret_cast<void*>('A'), "fifoA", 10,
                                          caps, 2);
        auto b = kos::thread::create_caps(fifo_worker, reinterpret_cast<void*>('B'), "fifoB", 10,
                                          caps, 2);
        TAP_CHECK(a.valid() and b.valid()); // spawn failure would hang the join below
        wait_n(2);
        TAP_CHECK(log_eq("AB"));
    }

    // --- Priority preempt on ready (thread-ctx sem post) -----------------------
    kos_cap_t g_go = KOS_CAP_NONE;
    void preempt_high(void*)
    {
        kos_sem_wait(CH_AUX); // g_go
        log_put('H');
        kos_sem_post(CH_DONE);
    }
    void preempt_low(void*)
    {
        log_put('l');
        kos_sem_post(CH_AUX); // g_go
        log_put('L');
        kos_sem_post(CH_DONE);
    }
    void t_preempt()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        log_reset();
        kos_sem_create(0, &g_go);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_go, CH_FULL}};
        auto hi = kos::thread::create_caps(preempt_high, nullptr, "high", 20, caps, 3);
        auto lo = kos::thread::create_caps(preempt_low, nullptr, "low", 8, caps, 3);
        TAP_CHECK(hi.valid() and lo.valid()); // spawn failure would hang the join below
        wait_n(2);
        kos_sem_destroy(g_go); // reclaim: the suite must be pool-honest (runs on MAX_SEMAPHORES=4)
        TAP_CHECK(log_eq("lHL"));
    }

    // --- Core clock read syscall -----------------------------------------------
    void t_cpu_clock_hz()
    {
        uint64_t hz = kos_cpu_clock_hz();
        TAP_CHECK(hz == kos_cpu_clock_hz());
        // 0 == the backend has no silicon core clock (host sim, QEMU virt); a real core
        // reports a plausible rate (>= 1 MHz, below every board's post-init clock).
        TAP_CHECK(hz == 0u or hz >= 1000000u);
    }

    void t_periph_clock_hz()
    {
        // A base no backend models returns 0 on EVERY target: the dispatch path and the
        // fallback plumbing reach the arch seam.
        uint32_t const bogus = kos_periph_clock_hz(0xDEAD0000u);
        TAP_CHECK(bogus == 0u);
        TAP_CHECK(bogus == kos_periph_clock_hz(0xDEAD0000u));
    }

    // An out-of-range port/pin is REJECTED on every target, before any hardware write:
    // -KOS_EINVAL where a chip owns its PORT/IOCR block, -KOS_ENOSYS on the declining-fallback
    // targets (host sim). The -KOS_EPERM exclusion is load-bearing: the AUTH_PINMUX gate runs
    // BEFORE the range check, so a bare `rc < 0` would also pass on a main that lost the bit.
    void t_pinmux_set()
    {
        int32_t const bad_port = kos_pinmux_set(99u, 0u, 0x10u);
        int32_t const bad_pin = kos_pinmux_set(0u, 99u, 0x10u);
        TAP_CHECK(bad_port < 0 and bad_port != -KOS_EPERM);
        TAP_CHECK(bad_pin < 0 and bad_pin != -KOS_EPERM);
    }

    // kos_cpu_clock_set is gated on AUTH_PSTATE: the gate returns the sentinel 0 ("cannot
    // change") to a caller that does not hold it, with NO retune. Neither main nor its child
    // holding no authority holds it.
    uint64_t g_clkset_low = 1;
    uint64_t g_clkset_mid = 1;
    uint64_t g_clkset_max = 1;
    kos_cap_t g_clkset_done = KOS_CAP_NONE;
    void clkset_unpriv_worker(void*) // UNPRIVILEGED; caps: g_clkset_done@1 (CH_DONE)
    {
        g_clkset_low = kos_cpu_clock_set(KOS_PSTATE_LOW);
        g_clkset_mid = kos_cpu_clock_set(KOS_PSTATE_MID);
        g_clkset_max = kos_cpu_clock_set(KOS_PSTATE_MAX);
        kos_sem_post(CH_DONE); // g_clkset_done (delegated from main)
    }
    void t_cpu_clock_set()
    {
        uint64_t const before = kos_cpu_clock_hz();
        uint64_t const t0 = kos_clock_now();
        g_clkset_low = 1;
        g_clkset_mid = 1;
        g_clkset_max = 1;
        kos_sem_create(0, &g_clkset_done);
        kos_cap_grant caps[] = {{g_clkset_done, CH_FULL}};
        auto w = kos::thread::create_caps(clkset_unpriv_worker, nullptr, "clkset", 10, caps, 1);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            kos_sem_destroy(g_clkset_done);
            return;
        }
        kos_sem_wait(g_clkset_done);
        kos_sem_destroy(g_clkset_done);
        TAP_CHECK(kos_cpu_clock_set(KOS_PSTATE_LOW) == 0u);
        TAP_CHECK(g_clkset_low == 0u);
        TAP_CHECK(g_clkset_mid == 0u);
        TAP_CHECK(g_clkset_max == 0u);
        TAP_CHECK(kos_cpu_clock_hz() == before);
        TAP_CHECK(kos_clock_now() >= t0);
    }

    // Every bit of a notification.
    constexpr uint32_t NOTE_ALL = 0xFFFFFFFFu;

    // --- A notification with no line and no KOS_AUTH_IRQ -----------------------
    void t_notify_no_line()
    {
        constexpr uint32_t BIT_A = 3;
        constexpr uint32_t BIT_B = 17;
        kos_cap_t note = KOS_CAP_NONE;
        TAP_CHECK(kos_notify_create(&note) == 0);
        // Two BADGED copies of one object, each reaching only the bit seated at its mint.
        kos_cap_t a = KOS_CAP_NONE;
        kos_cap_t b = KOS_CAP_NONE;
        TAP_CHECK(kos_notify_badge(note, BIT_A, &a) == 0);
        TAP_CHECK(kos_notify_badge(note, BIT_B, &b) == 0);
        // A badged copy is NOT a mint source: re-badging it would hand its holder the whole
        // object and the confinement would be vacuous.
        kos_cap_t again = KOS_CAP_NONE;
        TAP_CHECK(kos_notify_badge(a, BIT_B, &again) == -KOS_EACCES
                  and again == KOS_CAP_NONE);
        TAP_CHECK(kos_notify(a) == 0);
        TAP_CHECK(kos_notify(b) == 0);
        TAP_CHECK(kos_notify_bind(note) == 0);
        uint32_t bits = 0;
        // One wait, both bits: the object carries a word, not a single signal.
        TAP_CHECK(kos_notify_wait(note, NOTE_ALL, 0u, &bits) == 0);
        TAP_CHECK(bits == ((1u << BIT_A) | (1u << BIT_B)));
        // Drained: a second wait with no raise behind it can only time out.
        TAP_CHECK(kos_notify_wait(note, NOTE_ALL, 1000u, &bits) == -KOS_ETIMEDOUT);
        TAP_CHECK(kos_notify_unbind(note) == 0);
        TAP_CHECK(kos_handle_close(a) == 0);
        TAP_CHECK(kos_handle_close(b) == 0);
        TAP_CHECK(kos_handle_close(note) == 0);
    }

    // --- One bound thread per object, one object per thread --------------------
    Atomic<int32_t, Order::RELAXED> g_nbb_rc{-99};
    void nbb_second(void*) // caps: done@1, note(FULL)@2
    {
        g_nbb_rc = kos_notify_bind(2);
        kos_sem_post(CH_DONE);
    }
    void t_notify_bind_busy()
    {
        g_nbb_rc = -99;
        kos_cap_t note = KOS_CAP_NONE;
        TAP_CHECK(kos_notify_create(&note) == 0);
        TAP_CHECK(kos_notify_bind(note) == 0);
        // Rebinding the SAME object by the SAME thread is a no-op and must NOT take a second
        // reference.
        TAP_CHECK(kos_notify_bind(note) == 0);
        // A SECOND object for a thread already bound: the TCB names exactly one.
        kos_cap_t other = KOS_CAP_NONE;
        TAP_CHECK(kos_notify_create(&other) == 0);
        TAP_CHECK(kos_notify_bind(other) == -KOS_EBUSY);
        // And a second THREAD on the object main holds.
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {note, CH_FULL}};
        auto w = kos::thread::create_caps(nbb_second, nullptr, "nbb", 15, caps, 2);
        if (not w.valid())
        {
            TAP_CHECK(kos_notify_unbind(note) == 0);
            kos_handle_close(other);
            kos_handle_close(note);
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        TAP_CHECK(g_nbb_rc.load() == -KOS_EBUSY);
        // The control for both refusals: with the binding given up, the object takes one.
        TAP_CHECK(kos_notify_unbind(note) == 0);
        TAP_CHECK(kos_notify_bind(other) == 0);
        TAP_CHECK(kos_notify_unbind(other) == 0);
        TAP_CHECK(kos_handle_close(other) == 0);
        TAP_CHECK(kos_handle_close(note) == 0);
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // IRQ injection requires KICKOS_ENABLE_SELFTEST. Gate definitions and
    // registrations together or waits can hang without an injected event.
    // Use a line with no hardware source. ISR delivery uses the binding passed
    // as its argument, not the interrupted thread's capability table.
#if defined(KICKOS_IRQ_SOFT_ONLY_BASE)
    constexpr int IRQ_CTX_LINE = KICKOS_IRQ_SOFT_ONLY_BASE + 1;
#else
    constexpr int IRQ_CTX_LINE = KICKOS_IRQ_FREE_BASE + 10;
#endif
    // Attach `line` to a fresh notification and answer that notification's capability. The
    // capability is UNBADGED, which is the bit-0 one: irq_bind_notify copies the badge into
    // the binding, so this line raises bit 0. KOS_CAP_NONE on any refusal, with nothing left
    // open.
    kos_cap_t notify_for_line(kos_cap_t line)
    {
        kos_cap_t note = KOS_CAP_NONE;
        if (kos_notify_create(&note) != 0)
        {
            return KOS_CAP_NONE;
        }
        if (kos_irq_bind_notify(line, note) != 0)
        {
            kos_handle_close(note);
            return KOS_CAP_NONE;
        }
        return note;
    }

    void irq_waiter(void*)
    {
        auto note = kos::Notification::adopt(CH_NOTE);
        note.bind();
        note.wait(NOTE_ALL);
        log_put('W');
        kos_sem_post(CH_DONE);
    }
    void irq_injector(void*)
    {
        log_put('i');
        kos_irq_inject(IRQ_CTX_LINE);
        log_put('r');
        kos_sem_post(CH_DONE);
    }
    void t_irq()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        log_reset();
        // Stop on claim failure before spawning threads that would wait forever.
        kos_cap_t irq = KOS_CAP_NONE;
        TAP_CHECK(kos_irq_claim(IRQ_CTX_LINE, KOS_IRQ_EDGE, &irq) == 0);
        // The line must signal SOMEWHERE before it can be armed: an unattached line is one
        // kos_irq_ack refuses, because opening it would drop every raise.
        kos_cap_t note = notify_for_line(irq);
        TAP_CHECK(note != KOS_CAP_NONE);
        // Arm explicitly because injection may precede the waiter's first wait.
        kos_irq_ack(irq);
        kos_cap_grant wcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {irq, KOS_CAP_WAIT},
                                 {note, CH_FULL}};
        kos_cap_grant icaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto w = irq_spawn(irq_waiter, nullptr, "irqW", 15, wcaps, 4);
        auto inj = kos::thread::create_caps(irq_injector, nullptr, "irqI", 8, icaps, 2);
        TAP_CHECK(w.valid() and inj.valid()); // spawn failure would hang the join below
        wait_n(2);
        kos_handle_close(irq); // release the line after both workers exit
        kos_handle_close(note);
        TAP_CHECK(log_eq("iWr"));
    }

#endif // KICKOS_ENABLE_SELFTEST (IRQ-context delivery)

    // --- Round-robin preemption --------------------------------------------------
    constexpr uint64_t RR_GIVE_UP_NS = 5000000000ull;
    // Bumped by the spinner, sampled by the burner ACROSS its burn. Volatile: the burner
    // reads it twice with no intervening store of its own, and the spinner's whole body is
    // the increment.
    volatile uint64_t g_rr_spins = 0;
    volatile int g_rr_stop = 0;
    // What the burner saw the spinner do WHILE it burned.
    uint64_t g_rr_advanced = 0;

    // A stack sentinel per worker: the progress assertion cannot see a switch that resumes a
    // thread on the wrong stack, both threads still running.
    // Volatile and local so it lives on the stack, not in a callee-saved register.
    constexpr uint32_t RR_BURNER_SENTINEL = 0xB0570001u;
    constexpr uint32_t RR_SPINNER_SENTINEL = 0x5910572u;
    volatile int g_rr_stack_swap = 0;

    // This arm may not assert the order of two logged characters: two round-robin peers are
    // equal, so no post here preempts anything, and an order would rest on no slice exceeding
    // two quanta, which nothing establishes (a slice is the quantum plus the interrupt's
    // delivery latency).
    void rr_burner(void*) // caps: done@1, lock@2, gate@3
    {
        // Arrival, posted BEFORE the turnstile: the gate proves both workers were spawned,
        // not that both reached it.
        kos_sem_post(CH_DONE);
        stage_wait(3);
        volatile uint32_t me = RR_BURNER_SENTINEL;
        uint64_t const before = g_rr_spins;
        uint64_t const start = kos_clock_now();
        // Until the spinner has advanced, not for a fixed span: a loaded host can hold the
        // spinner's core longer than any span. RR_GIVE_UP_NS only turns a spinner that never
        // runs, the defect, into a failure instead of a hang.
        while (g_rr_spins == before and kos_clock_now() - start < RR_GIVE_UP_NS)
        {
            if (me != RR_BURNER_SENTINEL)
            {
                g_rr_stack_swap = 1;
                break;
            }
        }
        g_rr_advanced = g_rr_spins - before;
        g_rr_stop = 1;
        kos_sem_post(CH_DONE);
    }

    void rr_spinner(void*) // caps: done@1, lock@2, gate@3
    {
        kos_sem_post(CH_DONE);
        stage_wait(3);
        volatile uint32_t me = RR_SPINNER_SENTINEL;
        while (g_rr_stop == 0)
        {
            g_rr_spins = g_rr_spins + 1;
            if (me != RR_SPINNER_SENTINEL)
            {
                g_rr_stack_swap = 1;
                break;
            }
        }
        kos_sem_post(CH_DONE);
    }
    void t_rr()
    {
        // The quantum must be resolvable by the monotonic clock or the slice cannot preempt
        // mid-burn. Never hardcode a fine clock: the QEMU semihosting clock is coarse. The
        // probe must SPIN; a WFI would not advance the clock.
        uint64_t e0 = kos_clock_now();
        uint64_t e1 = e0;
        while (e1 == e0) { e1 = kos_clock_now(); }
        uint64_t e2 = e1;
        while (e2 == e1) { e2 = kos_clock_now(); }
        uint64_t granule = e2 - e1;
        uint64_t quantum = 1000000ull; // 1 ms on a fine clock (the shipped case)
        if (quantum < granule * 4)
        {
            quantum = granule * 4; // coarse clock: keep the slice well above a granule
        }
        g_rr_spins = 0;
        g_rr_stop = 0;
        g_rr_advanced = 0;
        g_rr_stack_swap = 0;

        kos_sem_create(0, &g_gate);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL},
                                {g_gate, CH_FULL}};
        // Must stay UNPRIVILEGED: that is what exercises the region reload per slice.
        auto a = kos::thread::create_caps(rr_burner, nullptr, "rrA", 10,
                                          caps, 3, KOS_POLICY_RR, static_cast<uint32_t>(quantum),
                                          /*privileged=*/false);
        auto b = kos::thread::create_caps(rr_spinner, nullptr, "rrB", 10,
                                          caps, 3, KOS_POLICY_RR, static_cast<uint32_t>(quantum),
                                          /*privileged=*/false);
        TAP_CHECK(g_gate != KOS_CAP_NONE and a.valid() and b.valid()); // spawn failure would hang the join below
        wait_n(2);
        stage_release();
        wait_n(2);
        kos_handle_close(g_gate);
        tap::diag("rr quantum %u ns, the other RR thread advanced %u during the burn",
                  static_cast<unsigned>(quantum), static_cast<unsigned>(g_rr_advanced));
        // Neither worker ever resumed holding the other's stack. FIRST, because a worker that
        // sees this abandons its loop: the burner then never burns and the progress check
        // below fails too, reporting a stack swap as "the other thread never ran".
        TAP_CHECK(g_rr_stack_swap == 0);
        // The other RR thread ran WHILE the burner burned. Sampled across the burn and not
        // at the end, so one that ran only before the burner started does not satisfy it.
        TAP_CHECK(g_rr_advanced > 0);
    }

    // --- Sleep ordering (tickless timer) ---------------------------------------
    // main spawns the 40 ms sleeper FIRST, so the two deadlines land in the intended order
    // only while the second spawn costs less than the 30 ms the requests differ by, and
    // nothing bounds a spawn. Each sleeper publishes the clock read taken just before its
    // kos_sleep_ns, and the arm judges the deadline order it can OBSERVE. The kernel exposes
    // no armed deadline, so the read-to-syscall gap stays as a residue.
    Atomic<uint32_t, Order::RELAXED> g_sleep_due_s{0};
    Atomic<uint32_t, Order::RELAXED> g_sleep_due_l{0};
    void sleeper(void* arg)
    {
        unsigned ms = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
        uint64_t const ns = static_cast<uint64_t>(ms) * 1000000ull;
        char c = 'L';
        if (ms < 20)
        {
            c = 'S';
        }
        uint32_t const due = stamp_due(ns);
        if (c == 'S')
        {
            g_sleep_due_s = due;
        }
        else
        {
            g_sleep_due_l = due;
        }
        kos_sleep_ns(ns);
        log_put(c);
        kos_sem_post(CH_DONE);
    }
    void t_sleep()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        log_reset();
        g_sleep_due_s = 0;
        g_sleep_due_l = 0;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto l = kos::thread::create_caps(sleeper, reinterpret_cast<void*>(uintptr_t{40}), "sleepL",
                                          10, caps, 2);
        auto s = kos::thread::create_caps(sleeper, reinterpret_cast<void*>(uintptr_t{10}), "sleepS",
                                          10, caps, 2);
        TAP_CHECK(l.valid() and s.valid()); // spawn failure would hang the join below
        wait_n(2);
        uint32_t const due_s = g_sleep_due_s;
        uint32_t const due_l = g_sleep_due_l;
        if (due_s == 0 or due_l == 0)
        {
            tap::skip("a sleeper reached its wake without stamping the deadline it asked for");
            return;
        }
        if (not stamp_before(due_s, due_l))
        {
            TAP_SKIP_VACUOUS("the 10 ms sleeper asked %u us AFTER the 40 ms one, so its deadline "
                             "is not the earlier one and no wake order is owed",
                             static_cast<unsigned>((due_s - due_l) / 1000u));
            return;
        }
        TAP_CHECK(log_eq("SL"));
    }

    // --- Two equal-priority threads blocking on one semaphore ------------------
    // The blocker must detach from the ready list before parking on the wait queue (they
    // share one link node), or the second waiter is orphaned and never wakes.
    kos_cap_t g_multi = KOS_CAP_NONE;
    void multi_worker(void* arg)
    {
        kos_sem_wait(CH_AUX); // g_multi
        log_put(arg_char(arg));
        kos_sem_post(CH_DONE);
    }
    void t_multi()
    {
        log_reset();
        kos_sem_create(0, &g_multi);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_multi, CH_FULL}};
        auto a = kos::thread::create_caps(multi_worker, reinterpret_cast<void*>('A'), "multiA", 10,
                                          caps, 3);
        auto b = kos::thread::create_caps(multi_worker, reinterpret_cast<void*>('B'), "multiB", 10,
                                          caps, 3);
        // A dropped spawn leaves no worker and main hangs in wait_n.
        TAP_CHECK(a.valid() and b.valid());
        kos_sleep_ns(5000000ull); // let both block on g_multi
        kos_sem_post(g_multi);
        kos_sem_post(g_multi);
        wait_n(2);
        kos_sem_destroy(g_multi); // reclaim
        TAP_CHECK(count('A') == 1 and count('B') == 1);
    }

#if defined(KICKOS_ENABLE_SELFTEST) // inject-driven (see the tier-2 block above)
    // --- Tier-1 IRQ-as-event: unprivileged userspace driver --------------------
    kos_cap_t g_irqdrv_done = KOS_CAP_NONE;
    // One ready-handshake handle shared by every tier-1 IRQ arm below; safe only because
    // they are strictly sequential and each creates and destroys it. Keep it shared: on
    // microbit the arena starts where .bss ends, so a handful of extra file-scope words
    // flips a later arena probe from RUN to SKIP.
    kos_cap_t g_irq_ready = KOS_CAP_NONE;
    void* g_mmio = nullptr; // fake device MMIO word, granted to the driver
    // Word 0 is the device, words 1..3 are what the driver saw. The page is the driver's
    // domain grant, making it a task of its own: an app global it wrote would be its own
    // copy, so the reading comes back only through this page.
    constexpr int IRQ_LINE = KICKOS_IRQ_FREE_BASE + 1;

    void irq_driver(void*)
    {
        auto irq = kos::Irq::adopt(CH_IRQ);
        auto note = kos::Notification::adopt(CH_NOTE);
        note.bind();
        volatile int* const dev = static_cast<volatile int*>(g_mmio);
        kos_sem_post(CH_READY); // g_irq_ready: bound to the object + about to park
        for (int i = 0; i < 3; i++)
        {
            note.wait(NOTE_ALL);
            dev[1 + i] = dev[0];
            irq.ack();
            kos_sem_post(CH_DONE); // g_irqdrv_done
        }
    }
    void t_irqdrv()
    {
        // main plays the device, so it needs write access to a page it allocated. App
        // static data will not do: it sits outside the arena and cannot be granted to the
        // driver.
        //
        // Alloc BEFORE the sems, or the alloc-fail early return leaks them.
        g_mmio = kos_ram_alloc(4096);
        if (g_mmio == nullptr)
        {
            tap::skip("4 KiB MMIO-page alloc failed, board too small");
            return;
        }
        // Without this grant the writes below fault: main does not reach its own arena
        // allocations.
        TAP_CHECK(kos_mem_self_grant(g_mmio, 4096, 0) == 0);
        for (int i = 0; i < 4; i++)
        {
            static_cast<volatile int*>(g_mmio)[i] = 0;
        }
        kos_sem_create(0, &g_irqdrv_done);
        kos_sem_create(0, &g_irq_ready);
        // main must mint the line (the suite declares KOS_AUTH_IRQ); a worker runs at
        // authority 0 and cannot claim for itself, so it gets a WAIT-only copy.
        kos_cap_t irq = KOS_CAP_NONE;
        TAP_CHECK(kos_irq_claim(IRQ_LINE, KOS_IRQ_EDGE, &irq) == 0);
        kos_cap_t note = notify_for_line(irq);
        TAP_CHECK(note != KOS_CAP_NONE);
        // A claim leaves the line MASKED and the ready handshake fires BEFORE the driver's
        // first wait, so arm the line here: otherwise an inject can land on a masked line
        // and the driver's first arm discards it.
        kos_irq_ack(irq);
        kos_cap_grant caps[] = {{g_irqdrv_done, CH_FULL},
                                {g_irq_ready, CH_FULL},
                                {irq, KOS_CAP_WAIT},
                                {note, CH_FULL}};
        auto drv = irq_spawn(irq_driver, nullptr, "irqdrv", 15, caps, 4,
                             KOS_POLICY_FIFO, 0, /*privileged=*/false, g_mmio, 4096);
        if (not drv.valid())
        {
            kos_sem_destroy(g_irqdrv_done); // reclaim before the failure return
            kos_sem_destroy(g_irq_ready);
        }
        TAP_CHECK(drv.valid()); // spawn failure would hang the ready handshake below
        // The driver is the sole holder of both: its exit frees the line, and the
        // notification with it.
        kos_handle_close(irq);
        kos_handle_close(note);
        kos_sem_wait(g_irq_ready);
        for (int i = 1; i <= 3; i++)
        {
            *static_cast<volatile int*>(g_mmio) = 0x100 + i;
            kos_irq_inject(IRQ_LINE);
            kos_sem_wait(g_irqdrv_done);
        }
        kos_sem_destroy(g_irqdrv_done); // reclaim
        kos_sem_destroy(g_irq_ready);
        volatile int const* const dev = static_cast<volatile int*>(g_mmio);
        TAP_CHECK(dev[1] == 0x101 and dev[2] == 0x102 and dev[3] == 0x103);
    }

    // Test coalescing while the driver holds the line masked between wake and ack.
    // main controls the release with a semaphore, independent of core count.
    // Readiness and ack-release tokens alternate on g_irq_ready.
    int g_mask_serviced = 0;
    constexpr int MASK_LINE = KICKOS_IRQ_FREE_BASE + 0;

    void mask_driver(void*)
    {
        auto irq = kos::Irq::adopt(CH_IRQ);
        auto note = kos::Notification::adopt(CH_NOTE);
        note.bind();
        kos_sem_post(CH_READY); // g_irq_ready
        for (int i = 0; i < 3; i++)
        {
            note.wait(NOTE_ALL);
            g_mask_serviced++;
            kos_sem_post(CH_DONE);  // phase A: serviced, line still MASKED, not yet acked
            kos_sem_wait(CH_READY); // main holds the masked window open
            irq.ack();
            kos_sem_post(CH_DONE);  // phase B: the line is ARMED again
        }
    }
    void t_irq_mask()
    {
        kos_sem_create(0, &g_irq_ready);
        g_mask_serviced = 0;
        kos_cap_t irq = KOS_CAP_NONE;
        if (kos_irq_claim(MASK_LINE, KOS_IRQ_EDGE, &irq) != 0)
        {
            kos_sem_destroy(g_irq_ready);
            tap::fail("the mask line could not be claimed");
            return;
        }
        kos_cap_t note = notify_for_line(irq);
        if (note == KOS_CAP_NONE)
        {
            kos_handle_close(irq);
            kos_sem_destroy(g_irq_ready);
            tap::fail("the mask line could not be attached to a notification");
            return;
        }
        kos_irq_ack(irq); // arm the freshly-claimed (masked) line
        kos_cap_grant caps[] = {{g_done, CH_FULL},
                                {g_irq_ready, CH_FULL},
                                {irq, KOS_CAP_WAIT},
                                {note, CH_FULL}};
        auto drv = irq_spawn(mask_driver, nullptr, "maskdrv", 1, caps, 4);
        if (not drv.valid())
        {
            kos_handle_close(irq);
            kos_handle_close(note);
            kos_sem_destroy(g_irq_ready);
            tap::fail("the mask driver did not spawn"); // its absence would hang the gate below
            return;
        }
        kos_handle_close(irq);
        kos_handle_close(note);
        kos_sem_wait(g_irq_ready); // driver is bound to the object, about to wait

        kos_irq_inject(MASK_LINE);
        wait_n(1); // phase A: serviced once, stopped before its ack, so masked from here
        kos_irq_inject(MASK_LINE);
        kos_irq_inject(MASK_LINE);
        kos_sem_post(g_irq_ready); // release the window; the driver acks
        // The latch half: a raise taken while the line was masked was kept and redelivered
        // at the ack. main injected nothing after the gate opened, so the second service can
        // only be a redelivery. Two posts: the ack's phase B, then that service's phase A.
        wait_n(2);
        bool const latched = (g_mask_serviced == 2);

        kos_sem_post(g_irq_ready);
        wait_n(1); // phase B: acked and ARMED, so a raise below cannot beat the ack
        bool one_deep = true;
#if KICKOS_KERNEL_CORES > 1
        // The one-deep half is not witnessed above one kernel core. Queueing rather than
        // coalescing shows up only as a service that must not happen, and a service that does
        // not happen raises no event to order a later read after. Gating the driver does not
        // help: a queued latch is consumed at the next gate release and is indistinguishable
        // there from the redelivery of a fresh raise. Checked on every one-core preset.
        tap::partial("one-deep coalescing is a service that must NOT happen");
#else
        kos_sleep_ns(2000000ull);
        one_deep = (g_mask_serviced == 2);
#endif
        // Liveness: a fresh raise reaches the re-armed line, and the driver's loop ends.
        kos_irq_inject(MASK_LINE);
        wait_n(1); // phase A of the third service
        bool const live = (g_mask_serviced == 3);
        kos_sem_post(g_irq_ready);
        wait_n(1); // phase B, which is also what says the driver is past its last gate
        kos_sem_destroy(g_irq_ready);

        TAP_CHECK(latched);
        TAP_CHECK(one_deep);
        TAP_CHECK(live);
    }

    // --- An EDGE driver can retire a latch it knows is stale -------------------
    // The controller is a reserved block no grant reaches, so kos_irq_discard is a driver's
    // ONLY way to drop a pending it knows is stale: ONE service where coalescing gives two.
    int g_disc_serviced = 0;
    // The one arm that RETIRES a pending: its line must have no source that can re-assert
    // underneath the ICPR write. RP2040 IRQ15 is SIO_IRQ_PROC0, which re-asserts from the
    // core-local FIFO level with no enable bit, and the retired latch redelivers. It also
    // needs a line of its own: the ownership arm leaves its line bound to a stale handle, so
    // sharing that one would make this arm depend on registration order.
#if defined(KICKOS_IRQ_SOFT_ONLY_BASE)
    constexpr int DISCARD_LINE = KICKOS_IRQ_SOFT_ONLY_BASE;
#else
    constexpr int DISCARD_LINE = KICKOS_IRQ_FREE_BASE + 9;
#endif

    static_assert(DISCARD_LINE < KICKOS_IRQ_FREE_BASE
                      or DISCARD_LINE > KICKOS_IRQ_FREE_BASE + 8,
                  "the discard line falls inside the selftest's nine-line IRQ block");
    static_assert(IRQ_CTX_LINE < KICKOS_IRQ_FREE_BASE
                      or IRQ_CTX_LINE > KICKOS_IRQ_FREE_BASE + 8,
                  "the irq-context line falls inside the selftest's nine-line IRQ block");
    static_assert(IRQ_CTX_LINE != DISCARD_LINE,
                  "the irq-context and discard arms would share a line");

    // Same held window as the mask arm: the driver stops between its wake and its discard
    // until main reopens the gate, so the raises main fires in between land on a line main
    // knows is masked. g_irq_ready carries both directions, strictly alternating.
    void discard_driver(void*)
    {
        auto irq = kos::Irq::adopt(CH_IRQ);
        auto note = kos::Notification::adopt(CH_NOTE);
        note.bind();
        kos_sem_post(CH_READY); // g_irq_ready
        for (int i = 0; i < 2; i++)
        {
            note.wait(NOTE_ALL);
            g_disc_serviced++;
            kos_sem_post(CH_DONE);  // phase A: serviced, MASKED, nothing retired yet
            kos_sem_wait(CH_READY); // main holds the masked window open
            irq.discard();          // retire anything coalesced onto the masked line
            irq.ack();
            kos_sem_post(CH_DONE);  // phase B: the line is ARMED again
        }
    }
    void t_irq_discard()
    {
        // A bad cap is refused at the same chokepoint as wait/ack, before any controller
        // write. Must stay ahead of every allocation, or its failure return strands them.
        TAP_CHECK(kos_irq_discard(KOS_CAP_NONE) == -KOS_EBADF);
        kos_sem_create(0, &g_irq_ready);
        g_disc_serviced = 0;
        kos_cap_t irq = KOS_CAP_NONE;
        if (kos_irq_claim(DISCARD_LINE, KOS_IRQ_EDGE, &irq) != 0)
        {
            kos_sem_destroy(g_irq_ready);
            tap::fail("the discard line could not be claimed");
            return;
        }
        kos_cap_t note = notify_for_line(irq);
        if (note == KOS_CAP_NONE)
        {
            kos_handle_close(irq);
            kos_sem_destroy(g_irq_ready);
            tap::fail("the discard line could not be attached to a notification");
            return;
        }
        kos_irq_ack(irq); // arm the freshly-claimed (masked) line
        kos_cap_grant caps[] = {{g_done, CH_FULL},
                                {g_irq_ready, CH_FULL},
                                {irq, KOS_CAP_WAIT},
                                {note, CH_FULL}};
        auto drv = irq_spawn(discard_driver, nullptr, "discirq", 1, caps, 4);
        if (not drv.valid())
        {
            kos_handle_close(irq);
            kos_handle_close(note);
            kos_sem_destroy(g_irq_ready);
            tap::fail("the discard driver did not spawn"); // its absence would hang the gate
            return;
        }
        kos_handle_close(irq);
        kos_handle_close(note);
        kos_sem_wait(g_irq_ready);

        kos_irq_inject(DISCARD_LINE);
        wait_n(1); // phase A: serviced once, stopped before its discard, so masked from here
        kos_irq_inject(DISCARD_LINE);
        kos_irq_inject(DISCARD_LINE);
        kos_sem_post(g_irq_ready); // release the window; the driver discards, then acks
        // Phase B is load-bearing here, not bookkeeping: a raise from main that beat the
        // discard would be retired by it, and the driver would then wait on a line nothing
        // will raise again.
        wait_n(1);
        bool retired = true;
#if KICKOS_KERNEL_CORES > 1
        // The retirement half is not witnessed above one kernel core. That the discard
        // dropped the coalesced latch shows up only as a second service that must not happen,
        // and a service that does not happen raises no event to order a later read after.
        // Checked on every one-core preset.
        tap::partial("a retired latch is a service that must NOT happen");
#else
        kos_sleep_ns(2000000ull);
        retired = (g_disc_serviced == 1);
#endif
        // Liveness, and this half holds on every core count: the discard must retire the
        // latch without wedging the line.
        kos_irq_inject(DISCARD_LINE);
        wait_n(1); // phase A of the second service
        bool const live = (g_disc_serviced == 2);
        kos_sem_post(g_irq_ready);
        wait_n(1); // phase B, which is also what says the driver is past its last gate
        kos_sem_destroy(g_irq_ready);

        TAP_CHECK(retired);
        TAP_CHECK(live);
    }

    // --- Auto-rearm: wait; service with no explicit ack ------------------------
    // A notify_wait re-arms every signaller CHAINED on the object whose bit it accepts, so a
    // driver that never acks still receives every subsequent IRQ. Driver MUST run above main,
    // so it reaches its next wait before main injects again.
    int g_autorearm_seen = 0;
    constexpr int AUTO_REARM_LINE = KICKOS_IRQ_FREE_BASE + 2;

    void autorearm_driver(void*)
    {
        // No Irq cap at all: this driver cannot ack even if it wanted to, so the rearm under
        // test can only be the one the wait issues over the bindings CHAINED on the object.
        auto note = kos::Notification::adopt(CH_NOTE);
        note.bind();
        kos_sem_post(CH_READY); // g_irq_ready
        for (int i = 0; i < 3; i++)
        {
            note.wait(NOTE_ALL); // no ack: the next wait re-arms the chained line
            g_autorearm_seen++;
            kos_sem_post(CH_DONE);
        }
    }
    void t_irq_autorearm()
    {
        kos_sem_create(0, &g_irq_ready);
        g_autorearm_seen = 0;
        kos_cap_t irq = KOS_CAP_NONE;
        TAP_CHECK(kos_irq_claim(AUTO_REARM_LINE, KOS_IRQ_EDGE, &irq) == 0);
        kos_cap_t note = notify_for_line(irq);
        TAP_CHECK(note != KOS_CAP_NONE);
        kos_irq_ack(irq); // arm the freshly-claimed (masked) line before injecting
        kos_cap_grant caps[] = {{g_done, CH_FULL},
                                {g_irq_ready, CH_FULL},
                                {note, CH_FULL}};
        uint16_t const dest[] = {CH_DONE, CH_READY, CH_NOTE};
        auto drv = irq_spawn(autorearm_driver, nullptr, "autoirq", 15, caps, 3,
                             KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr,
                             0, /*authority=*/0, dest);
        TAP_CHECK(drv.valid()); // spawn failure would hang the ready handshake below
        kos_handle_close(note);
        // main keeps the line until the arm is over: the driver holds no capability naming
        // it, so closing here would drop the last reference, unchain the signaller and take
        // the line away mid-arm.
        kos_sem_wait(g_irq_ready);
        kos_sem_destroy(g_irq_ready);
        for (int i = 0; i < 3; i++)
        {
            kos_irq_inject(AUTO_REARM_LINE);
            wait_n(1);
        }
        kos_handle_close(irq);
        TAP_CHECK(g_autorearm_seen == 3);
    }

    // --- No phantom wake in the ack;compute;wait shape -------------------------
    // After an explicit ack re-arms the line, exactly ONE injected event must yield exactly
    // ONE wait-return: the second wait BLOCKS. Setting needs_rearm in the ISR instead of on
    // wait-return unmasks early and phantom-posts. Driver MUST run below main so main
    // sequences each step, and every inject below must target an ARMED line.
    int g_phantom_seen = 0;
    constexpr int PHANTOM_LINE = KICKOS_IRQ_FREE_BASE + 4;

    void phantom_driver(void*)
    {
        auto irq = kos::Irq::adopt(CH_IRQ);
        auto note = kos::Notification::adopt(CH_NOTE);
        note.bind();
        kos_sem_post(CH_READY); // g_irq_ready
        note.wait(NOTE_ALL);
        irq.ack();
        kos_sem_post(CH_DONE); // acked; main injects the one mid-compute event
        note.wait(NOTE_ALL);
        g_phantom_seen++;
        kos_sem_post(CH_DONE);
        note.wait(NOTE_ALL);   // MUST block: only one event was injected, no phantom
        g_phantom_seen++;      // reached only on a phantom wake (the bug)
        kos_sem_post(CH_DONE);
    }
    void t_irq_phantom()
    {
        kos_sem_create(0, &g_irq_ready);
        g_phantom_seen = 0;
        kos_cap_t irq = KOS_CAP_NONE;
        TAP_CHECK(kos_irq_claim(PHANTOM_LINE, KOS_IRQ_EDGE, &irq) == 0);
        kos_cap_t note = notify_for_line(irq);
        TAP_CHECK(note != KOS_CAP_NONE);
        kos_irq_ack(irq); // arm: every inject below must target an ARMED line
        kos_cap_grant caps[] = {{g_done, CH_FULL},
                                {g_irq_ready, CH_FULL},
                                {irq, KOS_CAP_WAIT},
                                {note, CH_FULL}};
        auto drv = irq_spawn(phantom_driver, nullptr, "phantirq", 1, caps, 4); // below main
        TAP_CHECK(drv.valid()); // spawn failure would hang the ready handshake below
        kos_handle_close(irq);
        kos_handle_close(note);
        kos_sem_wait(g_irq_ready);
        kos_sem_destroy(g_irq_ready);

        kos_irq_inject(PHANTOM_LINE);
        wait_n(1);

        kos_irq_inject(PHANTOM_LINE); // the one mid-compute event, on the armed line
        wait_n(1);
        TAP_CHECK(g_phantom_seen == 1);

        // The driver is now parked in its third wait. It is lower priority, so
        // sleeping yields the CPU to it: a phantom wake would bump seen here.
        kos_sleep_ns(2000000ull);
        TAP_CHECK(g_phantom_seen == 1);

        // Wait is live (blocked, not lost) and the line re-armed itself.
        kos_irq_inject(PHANTOM_LINE);
        wait_n(1);
        TAP_CHECK(g_phantom_seen == 2);
    }

#endif // KICKOS_ENABLE_SELFTEST (tier-1 IRQ + mask)

    // --- Semaphore destroy: freelist reuse + generation-tagged handles ---------
    void t_sem_destroy()
    {
        kos_cap_t h = KOS_CAP_NONE;
        TAP_CHECK(kos_sem_create(0, &h) == 0);
        TAP_CHECK(kos_sem_destroy(h) == 0);
        TAP_CHECK(kos_sem_destroy(h) == -KOS_EBADF);
        kos_cap_t h2 = KOS_CAP_NONE;
        TAP_CHECK(kos_sem_create(0, &h2) == 0 and h2 != h); // reused slot carries a fresh generation
        TAP_CHECK(kos_sem_destroy(h2) == 0);
        // Malformed caps must fail with the SPECIFIC code -KOS_EBADF, not any negative.
        // wait/post share the same cap_resolve chokepoint.
        TAP_CHECK(kos_handle_close(KOS_CAP_NONE) == -KOS_EBADF);
        TAP_CHECK(kos_handle_close(0x7fffffff) == -KOS_EBADF);
        TAP_CHECK(kos_handle_close(0x00ffffff) == -KOS_EBADF);
        // The count is bounded at both ends: birth outside [0, KOS_SEM_COUNT_MAX] is
        // refused, and a post at the ceiling with no waiter is refused, not overflowed.
        kos_cap_t bad = KOS_CAP_NONE;
        TAP_CHECK(kos_sem_create(-1, &bad) == -KOS_EINVAL and bad == KOS_CAP_NONE);
        kos_cap_t hmax = KOS_CAP_NONE;
        TAP_CHECK(kos_sem_create(KOS_SEM_COUNT_MAX, &hmax) == 0
                  and kos_sem_post(hmax) == -KOS_EOVERFLOW
                  and kos_handle_close(hmax) == 0);
    }

    // --- Refcounted close of a DELEGATED sem: object survives while a co-holder is
    // parked; the last close frees it.
    kos_cap_t g_dsem = KOS_CAP_NONE;
    void destroy_waiter(void*) // caps: done@1, g_dsem@2 (CH_READY)
    {
        kos_sem_wait(CH_READY); // g_dsem: parks (initial 0)
        kos_sem_post(CH_DONE);
    }
    void destroy_poster(void*) // caps: done@1, g_dsem@2 (CH_READY)
    {
        // Sleep past MAIN's close below, then post: the wake of the parked waiter
        // happens strictly after MAIN has dropped its own (shared) cap on g_dsem.
        kos_sleep_ns(10000000ull);
        kos_sem_post(CH_READY);
        kos_sem_post(CH_DONE);
    }
    void t_sem_destroy_busy()
    {
        kos_sem_create(0, &g_dsem);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_dsem, CH_FULL}};
        auto w = kos::thread::create_caps(destroy_waiter, nullptr, "dwaiter", 15, caps, 2);
        auto p = kos::thread::create_caps(destroy_poster, nullptr, "dposter", 15, caps, 2);
        TAP_CHECK(w.valid() and p.valid()); // spawn failure would hang wait_n(2) below
        kos_sleep_ns(2000000ull);     // let the waiter park on g_dsem; refs = main+waiter+poster = 3
        // Close MAIN's cap while the waiter is parked and the poster has not yet posted:
        // refs 3->2, so the object must survive. A freed object or wait queue would leave
        // the poster's later post unable to wake the waiter, and wait_n(2) would hang.
        TAP_CHECK(kos_handle_close(g_dsem) == 0);
        wait_n(2);
        // Both holders have exited, so refs -> 0. Create/close well past the pool size
        // must never exhaust, which is only true if that last close reclaimed the slot.
        for (int i = 0; i < 100; i++)
        {
            kos_cap_t s = KOS_CAP_NONE;
            TAP_CHECK(kos_sem_create(0, &s) == 0 and kos_handle_close(s) == 0);
        }
    }

    // --- Owning kos::Semaphore RAII --------------------------------------------
    void t_sem_raii()
    {
        // Scoped create/destroy, well past the ~16-slot pool, must not exhaust it.
        for (int i = 0; i < 100; i++)
        {
            kos::Semaphore s;
            TAP_CHECK(s.valid());
        }
        // Move-construct empties the source, so scope exit destroys once.
        kos::Semaphore a;
        kos_cap_t aid = a.id();
        kos::Semaphore b(static_cast<kos::Semaphore&&>(a));
        TAP_CHECK(b.id() == aid and not a.valid());

        // Move-assign onto a live handle: the old target is destroyed, source emptied.
        kos::Semaphore c;
        c = static_cast<kos::Semaphore&&>(b);
        TAP_CHECK(c.id() == aid and not b.valid());

        // Self-move-assign is a no-op (must not destroy its own handle). Aliased
        // through a reference so the compiler's -Wself-move doesn't fire.
        kos::Semaphore& cref = c;
        c = static_cast<kos::Semaphore&&>(cref);
        TAP_CHECK(c.id() == aid);
    }

    // --- PI mutex: basic lock/unlock + mutual exclusion (H1) -------------------
    // The kos_yield() inside the critical section is load-bearing: without serialization
    // the peer reads the stale value and updates are lost, so conservation is the only pass.
    constexpr int MTX_ITERS = 20;
    int g_mtx_shared = 0;
    // A mutex cap carries CAP_TRANSFER only, so it must be delegated with a TRANSFER-only
    // mask: a CH_FULL mask is not a subset and delegation would reject it.
    constexpr uint8_t CH_MTX = KOS_CAP_TRANSFER;
    void mtx_basic_worker(void*) // caps: done@1, mutex@2
    {
        for (int i = 0; i < MTX_ITERS; i++)
        {
            kos_mutex_lock(2);      // the delegated mutex cap
            int tmp = g_mtx_shared;
            kos_yield();
            g_mtx_shared = tmp + 1;
            kos_mutex_unlock(2);
        }
        kos_sem_post(CH_DONE);
    }
    void t_mutex_basic()
    {
        kos_cap_t m = KOS_CAP_NONE;
        TAP_CHECK(kos_mutex_create(&m) == 0);
        g_mtx_shared = 0;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {m, CH_MTX}};
        auto a = kos::thread::create_caps(mtx_basic_worker, nullptr, "mbA", 10, caps, 2);
        auto b = kos::thread::create_caps(mtx_basic_worker, nullptr, "mbB", 10, caps, 2);
        auto c = kos::thread::create_caps(mtx_basic_worker, nullptr, "mbC", 10, caps, 2);
        if (not a.valid() or not b.valid() or not c.valid())
        {
            // The partial batch MUST be drained: they post the shared g_done, and a stale
            // post desyncs a later wait_n. Close the mutex too, or the leak cascades through
            // the cap table.
            int n = 0;
            if (a.valid()) { n++; }
            if (b.valid()) { n++; }
            if (c.valid()) { n++; }
            wait_n(n);
            kos_handle_close(m);
            tap::skip("pool too small");
            return;
        }
        wait_n(3);
        TAP_CHECK(kos_handle_close(m) == 0);
        TAP_CHECK(g_mtx_shared == 3 * MTX_ITERS);
    }

    // Hold times must allow a scheduling round trip (about 10-30 ms on ARMv6-M).
    // Use rendezvous for ordering assertions; time margins cannot guarantee order.
    uint64_t mtx_time_unit()
    {
        // A unit below a few granules is unmeasurable, so the granule is a lower bound.
        uint64_t g0 = kos_clock_now();
        uint64_t g1 = g0;
        while (g1 == g0) { g1 = kos_clock_now(); }
        uint64_t g2 = g1;
        while (g2 == g1) { g2 = kos_clock_now(); }
        uint64_t granule = g2 - g1;

        // Per-sleep OVERHEAD above a small real sleep: the jitter a 1-unit gap must out-scale.
        constexpr uint32_t N = 8;
        constexpr uint64_t probe = 200000ull; // 200 us
        uint64_t t0 = kos_clock_now();
        for (uint32_t i = 0; i < N; i++)
        {
            kos_sleep_ns(probe);
        }
        uint64_t rt = (kos_clock_now() - t0) / N;
        uint64_t overhead = 0;
        if (rt > probe)
        {
            overhead = rt - probe;
        }

        uint64_t unit = overhead * 32;
        uint64_t const gfloor = granule * 4;
        if (unit < gfloor)
        {
            unit = gfloor;
        }
        if (unit < 1000000ull)
        {
            unit = 1000000ull; // 1 ms floor
        }
        if (unit > 30000000ull)
        {
            unit = 30000000ull; // 30 ms cap: glitch guard
        }
        return unit;
    }
    void mtx_spin(uint64_t ns)
    {
        uint64_t start = kos_clock_now();
        while (kos_clock_now() - start < ns)
        {
        }
    }

    // Priority donation: low(8) holds the mutex, high(20) waits and boosts it,
    // and medium(12) must not preempt it. Semaphore posts reach the highest-priority
    // waiter, ordering low -> high -> medium.
    uint64_t g_mtx_unit = 1000000ull;
    kos_cap_t g_pi_go = KOS_CAP_NONE;
    void pi_low(void*) // caps: done@1, lock@2, mutex@3, gate@4, go@5
    {
        stage_wait(4);
        kos_mutex_lock(3);
        log_put('l');
        // High preempts at this post and blocks on the mutex before we return.
        kos_sem_post(5);
        log_put('u');
        kos_mutex_unlock(3);
        log_put('z');        // reached only after med (12) has run -> proves revert
        kos_sem_post(CH_DONE);
    }
    void pi_high(void*) // caps: done@1, lock@2, mutex@3, gate@4, go@5
    {
        stage_wait(4);
        kos_sem_wait(5);
        log_put('h');
        kos_sem_post(5); // medium becomes ready without preempting
        kos_mutex_lock(3); // low holds it -> block, boost low to 20
        log_put('H');
        kos_mutex_unlock(3);
        kos_sem_post(CH_DONE);
    }
    void pi_med(void*) // caps: done@1, lock@2, gate@3, go@4
    {
        stage_wait(3);
        kos_sem_wait(4);
        log_put('m');
        kos_sem_post(CH_DONE);
    }
    void t_mutex_pi()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        log_reset();
        kos_cap_t m = KOS_CAP_NONE;
        g_gate = KOS_CAP_NONE;
        g_pi_go = KOS_CAP_NONE;
        int const mrc = kos_mutex_create(&m);
        int const grc = kos_sem_create(0, &g_gate);
        int const orc = kos_sem_create(0, &g_pi_go);
        if (mrc != 0 or grc != 0 or orc != 0)
        {
            if (m != KOS_CAP_NONE) { kos_handle_close(m); }
            if (g_gate != KOS_CAP_NONE) { kos_handle_close(g_gate); }
            if (g_pi_go != KOS_CAP_NONE) { kos_handle_close(g_pi_go); }
            tap::skip("pool too small (1 mutex + 2 semaphores)");
            return;
        }
        kos_cap_grant lcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {m, CH_MTX},
                                 {g_gate, CH_FULL}, {g_pi_go, CH_FULL}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_gate, CH_FULL},
                                 {g_pi_go, CH_FULL}};
        auto lo = kos::thread::create_caps(pi_low, nullptr, "piLo", 8, lcaps, 5);
        auto hi = kos::thread::create_caps(pi_high, nullptr, "piHi", 20, lcaps, 5);
        auto md = kos::thread::create_caps(pi_med, nullptr, "piMd", 12, mcaps, 4);
        stage_release();
        if (not lo.valid() or not hi.valid() or not md.valid())
        {
            // Drain spawned workers and close the mutex. If low was not spawned,
            // main supplies the first token so the remaining workers can finish.
            int n = 0;
            if (lo.valid()) { n++; }
            if (hi.valid()) { n++; }
            if (md.valid()) { n++; }
            kos_sem_post(g_pi_go);
            wait_n(n);
            kos_handle_close(m);
            kos_handle_close(g_gate);
            kos_handle_close(g_pi_go);
            tap::skip("pool too small");
            return;
        }
        wait_n(3);
        kos_handle_close(g_gate);
        kos_handle_close(g_pi_go);
        TAP_CHECK(kos_handle_close(m) == 0);
        TAP_CHECK(count('l') == 1 and count('u') == 1 and count('h') == 1
                  and count('H') == 1 and count('m') == 1 and count('z') == 1);
        // High blocks only at the mutex; h before u proves it waited before unlock.
        TAP_CHECK(nth('h', 1) < nth('u', 1));
        TAP_CHECK(nth('u', 1) < nth('m', 1)); // BOOST: boosted low finished CS before med
        TAP_CHECK(nth('u', 1) < nth('H', 1)); // high acquired only after low released
        TAP_CHECK(nth('m', 1) < nth('z', 1)); // REVERT: low back at base, med ran first
    }

    // --- Chained/nested boost across two mutexes (H5) ---------------------------
    // A(20) waits on M1 owned by B(10); B waits on M2 owned by C(5). The boost must
    // PROPAGATE two hops, raising C to A's priority. D is ready while C spins, so 'e' before
    // 'd' can only hold if the boost travelled B -> C. Two semaphores are needed, a post
    // being popped by the HIGHEST-priority waiter; and A must be released by C, since a
    // release from B would reach A while B still merely holds M1 and the arm would witness
    // two single-hop walks a one-hop kernel reproduces.
    kos_cap_t g_ch_up = KOS_CAP_NONE; // C <-> B: M2 is held, then M1 is held and B is about to block
    kos_cap_t g_ch_on = KOS_CAP_NONE; // C -> A -> D: B is blocked on M2, then the chain has formed
    void ch_c(void*) // caps: done@1, lock@2, M2@3, gate@4, up@5, on@6
    {
        stage_wait(4);
        kos_mutex_lock(3); // M2
        log_put('c');
        kos_sem_post(5);   // M2 is held: B may take M1
        // B hands 5 back before blocking on M2, and B's block is what boosts us onto the CPU,
        // so getting past this wait means B is ALREADY a waiter on M2.
        kos_sem_wait(5);
        kos_sem_post(6);          // only now may A block on M1
        mtx_spin(g_mtx_unit * 8); // A blocks on M1 inside this
        log_put('e');
        kos_mutex_unlock(3);
        log_put('C');
        kos_sem_post(CH_DONE);
    }
    void ch_b(void*) // caps: done@1, lock@2, M1@3, M2@4, up@5
    {
        kos_sem_wait(5);
        kos_mutex_lock(3); // M1 (before A tries it)
        log_put('b');
        kos_sem_post(5);   // C is the only waiter and it is below us, so this does not preempt
        kos_mutex_lock(4); // M2: C holds it -> block, boost C to 10, which resumes C's wait
        kos_mutex_unlock(4);
        kos_mutex_unlock(3);
        kos_sem_post(CH_DONE);
    }
    void ch_a(void*) // caps: done@1, lock@2, M1@3, on@4
    {
        kos_sem_wait(4);
        kos_sem_post(4);   // releases D, which we outrank, so the chain forms before it runs
        kos_mutex_lock(3); // M1: B holds it AND waits on M2 -> boost B to 20, then C to 20
        kos_mutex_unlock(3);
        kos_sem_post(CH_DONE);
    }
    void ch_d(void*) // caps: done@1, lock@2, on@3
    {
        // Index 3, not the 4 A posts on: holding no mutex cap shifts the same semaphore one
        // slot down. Waiting on the wrong index returns at once and 'd' precedes 'e'.
        kos_sem_wait(3);
        log_put('d');
        kos_sem_post(CH_DONE);
    }
    void t_mutex_chain()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        // Ask the pool BEFORE creating anything: the three staging semaphores exceed the
        // supply on the small boards, so this stays a skip there instead of a create failure.
        if (not pool_can_host(4))
        {
            tap::skip("pool too small (4 interdependent workers)");
            return;
        }
        log_reset();
        g_mtx_unit = mtx_time_unit();
        kos_cap_t m1 = KOS_CAP_NONE;
        kos_cap_t m2 = KOS_CAP_NONE;
        int const rc1 = kos_mutex_create(&m1);
        int const rc2 = kos_mutex_create(&m2);
        TAP_CHECK(rc1 == 0 and rc2 == 0);
        TAP_CHECK(kos_sem_create(0, &g_gate) == 0);
        TAP_CHECK(kos_sem_create(0, &g_ch_up) == 0);
        TAP_CHECK(kos_sem_create(0, &g_ch_on) == 0);
        // Six grants, exactly KICKOS_MAX_SPAWN_GRANTS.
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {m2, CH_MTX},
                                 {g_gate, CH_FULL}, {g_ch_up, CH_FULL}, {g_ch_on, CH_FULL}};
        kos_cap_grant bcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL},
                                 {m1, CH_MTX}, {m2, CH_MTX}, {g_ch_up, CH_FULL}};
        kos_cap_grant acaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {m1, CH_MTX},
                                 {g_ch_on, CH_FULL}};
        kos_cap_grant dcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ch_on, CH_FULL}};
        auto c = kos::thread::create_caps(ch_c, nullptr, "chC", 5, ccaps, 6);
        auto b = kos::thread::create_caps(ch_b, nullptr, "chB", 10, bcaps, 5);
        auto a = kos::thread::create_caps(ch_a, nullptr, "chA", 20, acaps, 4);
        auto d = kos::thread::create_caps(ch_d, nullptr, "chD", 15, dcaps, 3);
        stage_release();
        // The probe above just held four slots and four stacks, so a failure now is a pool
        // bug, not a small board.
        TAP_CHECK(c.valid() and b.valid() and a.valid() and d.valid());
        wait_n(4);
        kos_handle_close(g_gate);
        kos_handle_close(g_ch_up);
        kos_handle_close(g_ch_on);
        TAP_CHECK(kos_handle_close(m1) == 0 and kos_handle_close(m2) == 0);
        TAP_CHECK(count('c') == 1 and count('e') == 1 and count('d') == 1
                  and count('b') == 1 and count('C') == 1);
        TAP_CHECK(nth('b', 1) < nth('e', 1)); // chain formed (B took M1 before C released M2)
        TAP_CHECK(nth('e', 1) < nth('d', 1)); // chain boost: C ran above med across two hops
    }

    // --- Owner dies holding the mutex: waiter gets OWNER_DIED (H7) --------------
    // The owner must exit WHILE still holding: that is what makes cap_teardown force-unlock
    // and the woken waiter's lock() return OWNER_DIED.
    int g_od_result = -99;
    // The owner's hold, and the instant the waiter reached its blocking lock. An owner that
    // exits first force-unlocks with nobody queued, the waiter's lock then answers a plain 0,
    // and that reading is indistinguishable from the defect this arm exists to catch.
    Window g_od_hold;
    Atomic<uint32_t, Order::RELAXED> g_od_lock_at{0};
    void od_owner(void*) // caps: mutex@1, holds@2
    {
        kos_mutex_lock(1);
        window_open(g_od_hold);
        // Twice: one token releases the waiter, one releases this arm's own body.
        kos_sem_post(2);
        kos_sem_post(2);
        kos_sleep_ns(g_mtx_unit * 3); // hold past the waiter's block, then exit owning
        window_close(g_od_hold);
        kos_exit(0);                  // exits still owning -> force-unlock
    }
    void od_waiter(void*) // caps: done@1, mutex@2, holds@3
    {
        // Gated on the owner's own post: above one core a sleep is no ordering, and a waiter
        // that reached the mutex first measures the inverse scenario. The owner's hold stays a
        // duration, a liveness margin for this thread to reach its blocking lock inside.
        kos_sem_wait(3);
        g_od_lock_at = stamp_now();
        g_od_result = kos_mutex_lock(2);
        // -KOS_EOWNERDEAD is a HELD acquire: unlock it too, or the robust mutex is stranded.
        // A plain `>= 0` test would skip this, since owner-died is a NEGATIVE code.
        if (g_od_result == 0 or g_od_result == -KOS_EOWNERDEAD)
        {
            kos_mutex_unlock(2);
        }
        kos_sem_post(CH_DONE);
    }
    void t_mutex_owner_died()
    {
        g_od_result = -99;
        window_reset(g_od_hold);
        g_od_lock_at = 0;
        g_mtx_unit = mtx_time_unit();
        kos_cap_t m = KOS_CAP_NONE;
        kos_cap_t holds = KOS_CAP_NONE;
        int const mrc = kos_mutex_create(&m);
        int const hrc = kos_sem_create(0, &holds);
        TAP_CHECK(mrc == 0 and hrc == 0);
        kos_cap_grant ocaps[] = {{m, CH_MTX}, {holds, CH_FULL}};
        kos_cap_grant wcaps[] = {{g_done, CH_FULL}, {m, CH_MTX}, {holds, CH_FULL}};
        auto ow = kos::thread::create_caps(od_owner, nullptr, "odOwn", 8, ocaps, 2);
        auto wt = kos::thread::create_caps(od_waiter, nullptr, "odWt", 12, wcaps, 3);
        TAP_CHECK(ow.valid() and wt.valid());
        kos_sem_wait(holds); // owner acquired the mutex (then sleeps, still holding)
        wait_n(1);           // only the waiter posts done (owner exited)
        uint32_t const lock_at = g_od_lock_at;
        if (not window_held(g_od_hold, lock_at))
        {
            kos_handle_close(m);
            kos_sem_destroy(holds);
            skip_window_lost("the waiter reaching its blocking lock while the owner still "
                             "held, which is what makes the force-unlock a HANDOFF",
                             g_od_hold, lock_at);
            return;
        }
        TAP_CHECK(g_od_result == -KOS_EOWNERDEAD);
        TAP_CHECK(kos_handle_close(m) == 0);
        kos_sem_destroy(holds);
    }

    // --- Deadlock refused -KOS_EDEADLK (H6): self-lock + a two-mutex wait cycle -
    int g_cyc_rb = -99;
    void cyc_a(void*) // caps: done@1, M1@2, M2@3, have1@4, goA@5
    {
        kos_mutex_lock(2); // M1
        kos_sem_post(4);   // have1
        kos_sem_wait(5);   // goA
        int r = kos_mutex_lock(3); // M2: B holds -> block; later handed off (r==0)
        if (r == 0)
        {
            kos_mutex_unlock(3);
        }
        kos_mutex_unlock(2);
        kos_sem_post(CH_DONE);
    }
    void cyc_b(void*) // caps: done@1, M2@2, M1@3, have2@4, goB@5
    {
        kos_mutex_lock(2); // M2
        kos_sem_post(4);   // have2
        kos_sem_wait(5);   // goB
        g_cyc_rb = kos_mutex_lock(3); // M1: closes the cycle -> refused
        if (g_cyc_rb == 0)
        {
            kos_mutex_unlock(3);
        }
        kos_mutex_unlock(2); // release M2 -> hands it to A
        kos_sem_post(CH_DONE);
    }
    // Keep the FIRST refusal of a batch: a later create cannot see a fuller supply than the
    // one before it, so the first is what binds this board.
    void note_refusal(int rc, int* first)
    {
        if (rc != 0 and *first == 0)
        {
            *first = rc;
        }
    }
    void t_mutex_deadlock()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        // Self-deadlock: a recursive lock is refused (-KOS_EDEADLK), not parked, and leaves
        // the mutex holdable/releasable normally.
        kos_cap_t self = KOS_CAP_NONE;
        TAP_CHECK(kos_mutex_create(&self) == 0);
        TAP_CHECK(kos_mutex_lock(self) == 0);
        TAP_CHECK(kos_mutex_lock(self) == -KOS_EDEADLK);
        TAP_CHECK(kos_mutex_unlock(self) == 0);
        TAP_CHECK(kos_handle_close(self) == 0);

        // Cross-thread cycle: A owns M1 + waits M2; B owns M2 + tries M1 -> -KOS_EDEADLK.
        g_cyc_rb = -99;
        kos_cap_t m1 = KOS_CAP_NONE;
        kos_cap_t m2 = KOS_CAP_NONE;
        kos_cap_t have1 = KOS_CAP_NONE;
        kos_cap_t have2 = KOS_CAP_NONE;
        kos_cap_t goA = KOS_CAP_NONE;
        kos_cap_t goB = KOS_CAP_NONE;
        int refused = 0;
        note_refusal(kos_mutex_create(&m1), &refused);
        note_refusal(kos_mutex_create(&m2), &refused);
        note_refusal(kos_sem_create(0, &have1), &refused);
        note_refusal(kos_sem_create(0, &have2), &refused);
        note_refusal(kos_sem_create(0, &goA), &refused);
        note_refusal(kos_sem_create(0, &goB), &refused);
        if (m1 == KOS_CAP_NONE or m2 == KOS_CAP_NONE or have1 == KOS_CAP_NONE
            or have2 == KOS_CAP_NONE or goA == KOS_CAP_NONE or goB == KOS_CAP_NONE)
        {
            // The cycle needs 2 mutexes and 4 sems live at once, which the small boards
            // cannot hold. No worker has spawned yet, so reclaiming in any order is safe.
            if (m1 != KOS_CAP_NONE) { kos_handle_close(m1); }
            if (m2 != KOS_CAP_NONE) { kos_handle_close(m2); }
            if (have1 != KOS_CAP_NONE) { kos_sem_destroy(have1); }
            if (have2 != KOS_CAP_NONE) { kos_sem_destroy(have2); }
            if (goA != KOS_CAP_NONE) { kos_sem_destroy(goA); }
            if (goB != KOS_CAP_NONE) { kos_sem_destroy(goB); }
            // Which supply ran out is the diagnosis, and the three have opposite fixes:
            // -KOS_EMFILE is this thread's capability table (widen the declared demand),
            // -KOS_EAGAIN is this TASK's ceiling with the pool itself still holding slots
            // (raise KICKOS_TASK_SEMAPHORE_BUDGET, and the pool with it), anything else is an
            // object pool genuinely out. Reporting the middle one as "pool too small" is the
            // mislabelled skip syscall-return-abi warns about.
            char const* why = "pool too small";
            if (refused == -KOS_EMFILE)
            {
                why = "cap table too small (6 concurrent caps)";
            }
            if (refused == -KOS_EAGAIN)
            {
                why = "task object budget too small (6 concurrent objects)";
            }
            tap::skip("%s", why);
            return;
        }
        kos_cap_grant acaps[] = {{g_done, CH_FULL}, {m1, CH_MTX}, {m2, CH_MTX},
                                 {have1, CH_FULL}, {goA, CH_FULL}};
        kos_cap_grant bcaps[] = {{g_done, CH_FULL}, {m2, CH_MTX}, {m1, CH_MTX},
                                 {have2, CH_FULL}, {goB, CH_FULL}};
        auto a = kos::thread::create_caps(cyc_a, nullptr, "cycA", 10, acaps, 5);
        auto b = kos::thread::create_caps(cyc_b, nullptr, "cycB", 10, bcaps, 5);
        TAP_CHECK(a.valid() and b.valid());
        kos_sem_wait(have1); // A owns M1
        kos_sem_wait(have2); // B owns M2
        kos_sem_post(goA);   // A tries M2 -> blocks (B owns it)
        kos_sem_post(goB);   // B tries M1 -> would cycle -> -KOS_EDEADLK, not parked
        wait_n(2);
        TAP_CHECK(g_cyc_rb == -KOS_EDEADLK);
        TAP_CHECK(kos_handle_close(m1) == 0 and kos_handle_close(m2) == 0);
        kos_sem_destroy(have1);
        kos_sem_destroy(have2);
        kos_sem_destroy(goA);
        kos_sem_destroy(goB);
    }

    // --- Closing a mutex you own is refused -------------------------------------
    void t_mutex_close_owned()
    {
        kos_cap_t m = KOS_CAP_NONE;
        TAP_CHECK(kos_mutex_create(&m) == 0);
        TAP_CHECK(kos_mutex_lock(m) == 0);
        TAP_CHECK(kos_handle_close(m) == -KOS_EBUSY);
        TAP_CHECK(kos_mutex_unlock(m) == 0);
        TAP_CHECK(kos_handle_close(m) == 0);
    }

    // B(6) holds M1 and M2; H(20) waits on M1 and boosts B. D(12) is ready
    // when B releases M2. B must retain H's donation rather than return to base
    // priority and let D run. Semaphore priority orders the setup.
    kos_cap_t g_mh_go = KOS_CAP_NONE;
    void mh_b(void*) // caps: done@1, lock@2, M1@3, M2@4, gate@5, go@6
    {
        stage_wait(5);
        kos_mutex_lock(3); // M1
        kos_mutex_lock(4); // M2
        log_put('b');
        // H preempts at this post and blocks on M1; D is ready behind it.
        kos_sem_post(6);
        kos_mutex_unlock(4); // release M2; H on M1 must keep B boosted
        log_put('x');
        kos_mutex_unlock(3); // release M1 -> hand to H, B drops to base
        kos_sem_post(CH_DONE);
    }
    void mh_h(void*) // caps: done@1, lock@2, M1@3, gate@4, go@5
    {
        stage_wait(4);
        kos_sem_wait(5);
        log_put('h');
        kos_sem_post(5); // D becomes ready without preempting
        kos_mutex_lock(3); // M1: B holds -> block, boost B to 20
        log_put('H');
        kos_mutex_unlock(3);
        kos_sem_post(CH_DONE);
    }
    void mh_d(void*) // caps: done@1, lock@2, gate@3, go@4
    {
        stage_wait(3);
        kos_sem_wait(4);
        log_put('d');
        kos_sem_post(CH_DONE);
    }
    void t_mutex_multi_held()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        log_reset();
        kos_cap_t m1 = KOS_CAP_NONE;
        kos_cap_t m2 = KOS_CAP_NONE;
        g_gate = KOS_CAP_NONE;
        g_mh_go = KOS_CAP_NONE;
        int const rc1 = kos_mutex_create(&m1);
        int const rc2 = kos_mutex_create(&m2);
        int const grc = kos_sem_create(0, &g_gate);
        int const orc = kos_sem_create(0, &g_mh_go);
        if (rc1 != 0 or rc2 != 0 or grc != 0 or orc != 0)
        {
            if (m1 != KOS_CAP_NONE) { kos_handle_close(m1); }
            if (m2 != KOS_CAP_NONE) { kos_handle_close(m2); }
            if (g_gate != KOS_CAP_NONE) { kos_handle_close(g_gate); }
            if (g_mh_go != KOS_CAP_NONE) { kos_handle_close(g_mh_go); }
            tap::skip("pool too small (2 mutexes + 2 semaphores)");
            return;
        }
        kos_cap_grant bcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL},
                                 {m1, CH_MTX}, {m2, CH_MTX}, {g_gate, CH_FULL},
                                 {g_mh_go, CH_FULL}};
        kos_cap_grant hcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {m1, CH_MTX},
                                 {g_gate, CH_FULL}, {g_mh_go, CH_FULL}};
        kos_cap_grant dcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_gate, CH_FULL},
                                 {g_mh_go, CH_FULL}};
        auto b = kos::thread::create_caps(mh_b, nullptr, "mhB", 6, bcaps, 6);
        auto h = kos::thread::create_caps(mh_h, nullptr, "mhH", 20, hcaps, 5);
        auto d = kos::thread::create_caps(mh_d, nullptr, "mhD", 12, dcaps, 4);
        stage_release();
        if (not b.valid() or not h.valid() or not d.valid())
        {
            // Drain spawned workers and close both mutexes. If B was not spawned,
            // main supplies the first token so the remaining workers can finish.
            int n = 0;
            if (b.valid()) { n++; }
            if (h.valid()) { n++; }
            if (d.valid()) { n++; }
            kos_sem_post(g_mh_go);
            wait_n(n);
            kos_handle_close(m1);
            kos_handle_close(m2);
            kos_handle_close(g_gate);
            kos_handle_close(g_mh_go);
            tap::skip("pool too small");
            return;
        }
        wait_n(3);
        kos_handle_close(g_gate);
        kos_handle_close(g_mh_go);
        TAP_CHECK(kos_handle_close(m1) == 0 and kos_handle_close(m2) == 0);
        TAP_CHECK(count('b') == 1 and count('h') == 1 and count('x') == 1
                  and count('H') == 1 and count('d') == 1);
        TAP_CHECK(nth('b', 1) < nth('h', 1)); // B owned both before H was let go
        // h before x proves H was waiting on M1 when B released M2.
        TAP_CHECK(nth('h', 1) < nth('x', 1));
        TAP_CHECK(nth('x', 1) < nth('H', 1)); // M2 released before M1 handed off
        TAP_CHECK(nth('H', 1) < nth('d', 1)); // RECOMPUTE: B stayed boosted, H ran before D
    }

    // --- unlock by a non-owner / of an unlocked mutex both refuse --------------
    // The owner check is reachable from an untrusted caller, so it must return an
    // error code here, never panic.
    int g_nonowner_rc = -99;
    void nonowner_unlock(void*) // caps: done@1, mutex@2
    {
        g_nonowner_rc = kos_mutex_unlock(2);
        kos_sem_post(CH_DONE);
    }
    void t_mutex_unlock_errors()
    {
        kos_cap_t m = KOS_CAP_NONE;
        TAP_CHECK(kos_mutex_create(&m) == 0);
        TAP_CHECK(kos_mutex_unlock(m) == -KOS_EPERM); // unlocked: caller is not the (null) owner
        TAP_CHECK(kos_mutex_lock(m) == 0);
        g_nonowner_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {m, CH_MTX}};
        auto w = kos::thread::create_caps(nonowner_unlock, nullptr, "nonown", 10, caps, 2);
        TAP_CHECK(w.valid());
        wait_n(1);
        TAP_CHECK(g_nonowner_rc == -KOS_EPERM);
        TAP_CHECK(kos_mutex_unlock(m) == 0);
        TAP_CHECK(kos_handle_close(m) == 0);
    }

    // --- Owner dies holding with NO waiter: m->owner cleared, re-lockable -------
    void od_solo_owner(void*) // caps: mutex@1, holds@2
    {
        kos_mutex_lock(1);
        kos_sem_post(2); // holds
        kos_exit(0);     // exits owning, no waiter -> force-unlock nulls m->owner
    }
    void t_mutex_owner_died_nowaiter()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        kos_cap_t m = KOS_CAP_NONE;
        kos_cap_t holds = KOS_CAP_NONE;
        int const mrc = kos_mutex_create(&m);
        int const hrc = kos_sem_create(0, &holds);
        TAP_CHECK(mrc == 0 and hrc == 0);
        kos_cap_grant ocaps[] = {{m, CH_MTX}, {holds, CH_FULL}};
        auto ow = kos::thread::create_caps(od_solo_owner, nullptr, "odSolo", 8, ocaps, 2);
        TAP_CHECK(ow.valid());
        kos_sem_wait(holds); // owner acquired, then exits (higher prio, runs to exit)
        // If force-unlock did not null m->owner, this lock would block forever on a
        // dead owner. It must acquire cleanly (fresh, uncontended -> 0).
        TAP_CHECK(kos_mutex_lock(m) == 0);
        TAP_CHECK(kos_mutex_unlock(m) == 0);
        TAP_CHECK(kos_handle_close(m) == 0);
        kos_sem_destroy(holds);
    }

    // --- Delegated-mutex refcount: child closes its cap, parent still locks ------
    void deleg_closer(void*) // caps: done@1, mutex@2
    {
        kos_handle_close(2);   // drop the child's delegated cap (refs 2 -> 1)
        kos_sem_post(CH_DONE);
    }
    void t_mutex_deleg_refcount()
    {
        kos_cap_t m = KOS_CAP_NONE; // refs = 1 (main)
        TAP_CHECK(kos_mutex_create(&m) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {m, CH_MTX}};  // refs -> 2
        auto w = kos::thread::create_caps(deleg_closer, nullptr, "delcl", 10, caps, 2);
        TAP_CHECK(w.valid());
        wait_n(1);
        // Child closed its cap (and exited): the object must survive on main's cap.
        TAP_CHECK(kos_mutex_lock(m) == 0);
        TAP_CHECK(kos_mutex_unlock(m) == 0);
        TAP_CHECK(kos_handle_close(m) == 0);
        // Pool honesty: create/close well past the pool must not exhaust.
        for (int i = 0; i < 40; i++)
        {
            kos_cap_t x = KOS_CAP_NONE;
            TAP_CHECK(kos_mutex_create(&x) == 0 and kos_handle_close(x) == 0);
        }
    }

    // --- A thread sets its own priority ----------------------------------------------------
    // S starts below P and raises itself above it, so readying P does not preempt S ('r'
    // first). S then lowers itself below P, which must hand P the CPU before the call returns
    // ('p' before 'l').
    constexpr uint8_t SP_LOW = 6;
    constexpr uint8_t SP_PEER = 8;
    constexpr uint8_t SP_HIGH = 10;
    kos_cap_t g_sp_go = KOS_CAP_NONE;
    int g_sp_raise = -99;
    int g_sp_lower = -99;
    void sp_self(void*) // caps: done@1, lock@2, gate@3, go@4
    {
        stage_wait(3);
        g_sp_raise = kos_thread_set_priority(SP_HIGH);
        kos_sem_post(4);
        log_put('r');
        g_sp_lower = kos_thread_set_priority(SP_LOW);
        log_put('l');
        kos_sem_post(CH_DONE);
    }
    void sp_peer(void*) // caps: done@1, lock@2, gate@3, go@4
    {
        stage_wait(3);
        kos_sem_wait(4);
        log_put('p');
        kos_sem_post(CH_DONE);
    }
    void t_prio_self_raise_lower()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        log_reset();
        g_sp_raise = -99;
        g_sp_lower = -99;
        g_gate = KOS_CAP_NONE;
        g_sp_go = KOS_CAP_NONE;
        int const grc = kos_sem_create(0, &g_gate);
        int const orc = kos_sem_create(0, &g_sp_go);
        if (grc != 0 or orc != 0)
        {
            if (g_gate != KOS_CAP_NONE) { kos_handle_close(g_gate); }
            if (g_sp_go != KOS_CAP_NONE) { kos_handle_close(g_sp_go); }
            tap::skip("pool too small (2 semaphores)");
            return;
        }
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_gate, CH_FULL},
                                {g_sp_go, CH_FULL}};
        auto s = kos::thread::create_caps(sp_self, nullptr, "spSelf", SP_LOW, caps, 4);
        auto p = kos::thread::create_caps(sp_peer, nullptr, "spPeer", SP_PEER, caps, 4);
        stage_release();
        if (not s.valid() or not p.valid())
        {
            // The peer may be parked on `go` with no self to post it.
            int n = 0;
            if (s.valid()) { n++; }
            if (p.valid()) { n++; }
            kos_sem_post(g_sp_go);
            wait_n(n);
            kos_handle_close(g_gate);
            kos_handle_close(g_sp_go);
            tap::skip("pool too small");
            return;
        }
        wait_n(2);
        kos_handle_close(g_gate);
        kos_handle_close(g_sp_go);
        TAP_CHECK(g_sp_raise == 0);
        TAP_CHECK(g_sp_lower == 0);
        TAP_CHECK(count('r') == 1 and count('p') == 1 and count('l') == 1);
        TAP_CHECK(nth('r', 1) < nth('p', 1)); // RAISE: P readied below S stayed off the CPU
        TAP_CHECK(nth('p', 1) < nth('l', 1)); // LOWER: P ran before S's call returned
    }

    // L holds the mutex and H waits on it, boosting L to H's priority. L lowers its own base
    // while boosted: it keeps the boost, so M, readied before, stays off the CPU until L unlocks
    // ('u' before 'm'). Unlocked, L falls to the new base, below K as well as M ('k' before
    // 'z'); at its old base it would have run ahead of K.
    constexpr uint8_t PB_BASE = 4;
    constexpr uint8_t PB_K = 6;
    constexpr uint8_t PB_LOW = 8;
    constexpr uint8_t PB_MED = 12;
    constexpr uint8_t PB_HIGH = 20;
    kos_cap_t g_pb_go = KOS_CAP_NONE;
    int g_pb_set = -99;
    void pb_low(void*) // caps: done@1, lock@2, mutex@3, gate@4, go@5
    {
        stage_wait(4);
        kos_mutex_lock(3);
        log_put('l');
        // H preempts at this post and blocks on the mutex before we return.
        kos_sem_post(5);
        g_pb_set = kos_thread_set_priority(PB_BASE);
        log_put('u');
        kos_mutex_unlock(3);
        log_put('z');
        kos_sem_post(CH_DONE);
    }
    void pb_high(void*) // caps: done@1, lock@2, mutex@3, gate@4, go@5
    {
        stage_wait(4);
        kos_sem_wait(5);
        log_put('h');
        // M takes the first token and K, still short of its own wait, the banked second.
        kos_sem_post(5);
        kos_sem_post(5);
        kos_mutex_lock(3); // L holds it -> block, boost L to 20
        log_put('H');
        kos_mutex_unlock(3);
        kos_sem_post(CH_DONE);
    }
    void pb_waiter(void* arg) // caps: done@1, lock@2, gate@3, go@4
    {
        stage_wait(3);
        kos_sem_wait(4);
        log_put(arg_char(arg));
        kos_sem_post(CH_DONE);
    }
    void t_prio_self_boosted()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        log_reset();
        g_pb_set = -99;
        kos_cap_t m = KOS_CAP_NONE;
        g_gate = KOS_CAP_NONE;
        g_pb_go = KOS_CAP_NONE;
        int const mrc = kos_mutex_create(&m);
        int const grc = kos_sem_create(0, &g_gate);
        int const orc = kos_sem_create(0, &g_pb_go);
        if (mrc != 0 or grc != 0 or orc != 0)
        {
            if (m != KOS_CAP_NONE) { kos_handle_close(m); }
            if (g_gate != KOS_CAP_NONE) { kos_handle_close(g_gate); }
            if (g_pb_go != KOS_CAP_NONE) { kos_handle_close(g_pb_go); }
            tap::skip("pool too small (1 mutex + 2 semaphores)");
            return;
        }
        kos_cap_grant lcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {m, CH_MTX},
                                 {g_gate, CH_FULL}, {g_pb_go, CH_FULL}};
        kos_cap_grant wcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_gate, CH_FULL},
                                 {g_pb_go, CH_FULL}};
        void* const med_tag = reinterpret_cast<void*>(static_cast<uintptr_t>('m'));
        void* const k_tag = reinterpret_cast<void*>(static_cast<uintptr_t>('k'));
        auto lo = kos::thread::create_caps(pb_low, nullptr, "pbLo", PB_LOW, lcaps, 5);
        auto hi = kos::thread::create_caps(pb_high, nullptr, "pbHi", PB_HIGH, lcaps, 5);
        auto md = kos::thread::create_caps(pb_waiter, med_tag, "pbMd", PB_MED, wcaps, 4);
        auto kw = kos::thread::create_caps(pb_waiter, k_tag, "pbK", PB_K, wcaps, 4);
        stage_release();
        if (not lo.valid() or not hi.valid() or not md.valid() or not kw.valid())
        {
            // Whichever waiters are parked on `go` get a token each, the two H would post.
            int n = 0;
            if (lo.valid()) { n++; }
            if (hi.valid()) { n++; }
            if (md.valid()) { n++; }
            if (kw.valid()) { n++; }
            kos_sem_post(g_pb_go);
            kos_sem_post(g_pb_go);
            kos_sem_post(g_pb_go);
            wait_n(n);
            kos_handle_close(m);
            kos_handle_close(g_gate);
            kos_handle_close(g_pb_go);
            tap::skip("pool too small");
            return;
        }
        wait_n(4);
        kos_handle_close(g_gate);
        kos_handle_close(g_pb_go);
        TAP_CHECK(kos_handle_close(m) == 0);
        TAP_CHECK(g_pb_set == 0);
        TAP_CHECK(count('l') == 1 and count('h') == 1 and count('u') == 1 and count('H') == 1
                  and count('m') == 1 and count('k') == 1 and count('z') == 1);
        TAP_CHECK(nth('h', 1) < nth('u', 1)); // H waited on the mutex before the lowering
        TAP_CHECK(nth('u', 1) < nth('m', 1)); // BOOST KEPT: lowering the base left L at 20
        TAP_CHECK(nth('u', 1) < nth('H', 1));
        TAP_CHECK(nth('m', 1) < nth('z', 1));
        TAP_CHECK(nth('k', 1) < nth('z', 1)); // NEW BASE: L fell below K once unlocked
    }

    // --- The userspace SPSC byte ring ------------------------------------------
    // Buffer and ring MUST stay on the stack: a static here comes out of the tiny boards'
    // user arena and pushes a later arena probe into a skip.
    void t_byte_ring()
    {
        unsigned char buf[8];
        struct kos_byte_ring r;
        kos_byte_ring_init(&r, buf, sizeof(buf));
        // Capacity is size-1: one slot is reserved so head == tail is unambiguously empty.
        TAP_CHECK(kos_byte_ring_used(&r) == 0);
        TAP_CHECK(kos_byte_ring_space(&r) == 7);

        unsigned char const src[4] = {'a', 'b', 'c', 'd'};
        TAP_CHECK(kos_byte_ring_push(&r, src, 4) == 4);
        TAP_CHECK(kos_byte_ring_used(&r) == 4);
        TAP_CHECK(kos_byte_ring_space(&r) == 3);

        // A short accept, NOT an error and NOT a silent drop: the caller decides.
        TAP_CHECK(kos_byte_ring_push(&r, src, 4) == 3);
        TAP_CHECK(kos_byte_ring_space(&r) == 0);
        TAP_CHECK(kos_byte_ring_push(&r, src, 1) == 0);

        unsigned char out[8] = {0};
        TAP_CHECK(kos_byte_ring_pop(&r, out, 2) == 2);
        TAP_CHECK(out[0] == 'a' and out[1] == 'b');
        // Wrap: the two pushed below straddle the end of the buffer, so a mask bug shows
        // up as wrong ORDER here rather than as a bad count.
        TAP_CHECK(kos_byte_ring_push(&r, src, 2) == 2);
        unsigned char one = 0;
        TAP_CHECK(kos_byte_ring_pop_one(&r, &one) == 1);
        TAP_CHECK(one == 'c');
        TAP_CHECK(kos_byte_ring_pop(&r, out, sizeof(out)) == 6);
        TAP_CHECK(out[0] == 'd' and out[1] == 'a' and out[2] == 'b' and out[3] == 'c');
        TAP_CHECK(out[4] == 'a' and out[5] == 'b');
        TAP_CHECK(kos_byte_ring_used(&r) == 0);
        TAP_CHECK(kos_byte_ring_pop_one(&r, &one) == 0);

        // A non-power-of-two size is a programming error and is REFUSED rather than
        // masked wrong: the ring reports empty-and-full instead of corrupting memory.
        struct kos_byte_ring bad;
        kos_byte_ring_init(&bad, buf, 6);
        TAP_CHECK(kos_byte_ring_space(&bad) == 0);
        TAP_CHECK(kos_byte_ring_push(&bad, src, 1) == 0);
    }

    // --- Per-grant destination indices: the refusals ---------------------------
    // A bad placement list must be REFUSED before anything is built.
    // Both spawns below must fail, so this body is deliberately never reached.
    void capdest_never_runs(void*) { kos_exit(0); }

    // Posts the completion sem from a NON-default index. If placement were ignored the post
    // would fail and main would never be released, so the failure surfaces as a TRUNCATED
    // run rather than a `not ok`. Do not add a report channel: two more file-scope words
    // starve microbit's arena.
    void capdest_probe(void*) { kos_sem_post(CH_IRQ); }

    void t_cap_dest()
    {
        kos_cap_t sem = KOS_CAP_NONE;
        TAP_CHECK(kos_sem_create(0, &sem) == 0);
        kos_cap_grant caps[] = {{sem, CH_FULL}, {sem, CH_FULL}};

        // Two grants naming one slot: the second install would overwrite the first and
        // leak its reference, so the list is refused whole.
        uint16_t const collide[] = {CH_DONE, CH_DONE};
        TAP_CHECK(kos::thread::create_caps(capdest_never_runs, nullptr, "cd1", 10, caps, 2,
                                           KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                           collide)
                      .error() == -KOS_EINVAL);

        // A destination past the child's table. cap.h caps KICKOS_MAX_HANDLES at
        // KCAP_RESERVED_INDEX, so index 65535 is out of range on every board.
        uint16_t const far_off[] = {CH_DONE, 65535};
        TAP_CHECK(kos::thread::create_caps(capdest_never_runs, nullptr, "cd2", 10, caps, 2,
                                           KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                           far_off)
                      .error() == -KOS_EINVAL);

        // A collision with a DEFAULTED entry counts too: entry 0 defaults to index 1 and
        // entry 1 names it explicitly.
        uint16_t const vs_default[] = {0, CH_DONE};
        TAP_CHECK(kos::thread::create_caps(capdest_never_runs, nullptr, "cd3", 10, caps, 2,
                                           KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                           vs_default)
                      .error() == -KOS_EINVAL);

        // The POSITIVE half: delegate the completion sem at index 3 with nothing at 1 or 2.
        // Ignoring the destination puts it at 1 and the worker's post never lands.
        kos_cap_grant one[] = {{g_done, CH_FULL}};
        uint16_t const at3[] = {CH_IRQ};
        auto const w = kos::thread::create_caps(capdest_probe, nullptr, "cdp", 15, one, 1,
                                                KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                                at3);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            kos_sem_destroy(sem);
            return;
        }
        wait_n(1);
        kos_sem_destroy(sem);
    }

    // --- The published console's CRLF expansion --------------------------------
    // Must run on EVERY board, including the ones that do not cook: the cook is #if
    // KICKOS_CONSOLE_CRLF, so otherwise the expansion ships with coverage on no board at
    // all. Keep it pure stack: microbit's arena is 16 KiB and a `static` comes out of it.
    void t_console_crlf()
    {
        unsigned char out[16];
        uint32_t taken = 0;

        // Identity when there is nothing to expand.
        unsigned char const plain[3] = {'a', 'b', 'c'};
        TAP_CHECK(kickos::console::cook_crlf(plain, 3, out, sizeof(out), &taken) == 3);
        TAP_CHECK(taken == 3);
        TAP_CHECK(out[0] == 'a' and out[1] == 'b' and out[2] == 'c');

        // Every '\n' gains a '\r' before it.
        unsigned char const nl[3] = {'a', '\n', 'b'};
        TAP_CHECK(kickos::console::cook_crlf(nl, 3, out, sizeof(out), &taken) == 4);
        TAP_CHECK(taken == 3);
        TAP_CHECK(out[0] == 'a' and out[1] == '\r' and out[2] == '\n' and out[3] == 'b');

        // The rule does NOT look back: an input that already carries "\r\n" becomes
        // "\r\r\n". A doubled CR is a no-op on the wire, and this matches kconsole_write.
        unsigned char const crnl[2] = {'\r', '\n'};
        TAP_CHECK(kickos::console::cook_crlf(crnl, 2, out, sizeof(out), &taken) == 3);
        TAP_CHECK(out[0] == '\r' and out[1] == '\r' and out[2] == '\n');

        // A '\n' is never split from its '\r' at the chunk boundary: with room for one
        // more byte the expansion stops BEFORE it, so `taken` is a clean resume point.
        unsigned char const tight[2] = {'x', '\n'};
        TAP_CHECK(kickos::console::cook_crlf(tight, 2, out, 2, &taken) == 1);
        TAP_CHECK(taken == 1);
        TAP_CHECK(out[0] == 'x');
        // Resuming from there emits the pair whole.
        TAP_CHECK(kickos::console::cook_crlf(tight + taken, 1, out, 2, &taken) == 2);
        TAP_CHECK(taken == 1);
        TAP_CHECK(out[0] == '\r' and out[1] == '\n');

        // No output room at all consumes nothing rather than dropping an input byte.
        TAP_CHECK(kickos::console::cook_crlf(plain, 3, out, 0, &taken) == 0);
        TAP_CHECK(taken == 0);

        // The write policy KOS_UART_SET_MODE carries.
        kickos::Atomic<uint32_t, kickos::Order::RELAXED> mode{0};
        // A service with no unframed console arm REFUSES: a stored mode nothing reads would
        // tell a caller byte loss was enabled while its writes still blocked.
        TAP_CHECK(kickos::console::mode_apply(nullptr, KOS_UART_F_NONBLOCK, 0u)
                  == -KOS_ENOSYS);
        // An unknown bit is refused whole rather than masked, and stores nothing.
        TAP_CHECK(kickos::console::mode_apply(&mode, 0x80u, 0u) == -KOS_EINVAL);
        TAP_CHECK(mode == 0);
        // Accepted and readable back, which is what the console arm consults per write.
        TAP_CHECK(kickos::console::mode_apply(&mode, KOS_UART_F_NONBLOCK, 0u) == 0);
        TAP_CHECK(mode == KOS_UART_F_NONBLOCK);
        // Clearing it restores the paced default; the flag is not a one-way latch.
        TAP_CHECK(kickos::console::mode_apply(&mode, 0u, 0u) == 0);
        TAP_CHECK(mode == 0);
        // A transport that cannot honour a blocking write REQUIRES the flag and refuses to
        // clear it, so a caller is told it cannot have back-pressure instead of being handed
        // an unbounded wait. The refusal stores nothing.
        mode = KOS_UART_F_NONBLOCK;
        TAP_CHECK(kickos::console::mode_apply(&mode, 0u, KOS_UART_F_NONBLOCK)
                  == -KOS_ENOTSUP);
        TAP_CHECK(mode == KOS_UART_F_NONBLOCK);
        TAP_CHECK(kickos::console::mode_apply(&mode, KOS_UART_F_NONBLOCK,
                                             KOS_UART_F_NONBLOCK) == 0);

        // A non-blocking write REPORTS its short accept: the service arm turns the shortfall
        // into stats.tx_dropped, the only channel an unframed writer can read.
        unsigned char rbuf[8];
        struct kos_byte_ring ring;
        kos_byte_ring_init(&ring, rbuf, sizeof(rbuf));
        struct kos_uart_stats st = {};
        unsigned char twelve[12];
        memset(twelve, 'z', sizeof(twelve));
        // Eight bytes of room for twelve offered: a short accept, not a refusal and not a
        // wait.
        uint32_t const nb = kickos::console::write_console(&ring, &st, twelve, 12,
                                                          KOS_UART_F_NONBLOCK);
        TAP_CHECK(nb < 12);
        TAP_CHECK(12u - nb > 0u); // the shortfall the service arm charges to tx_dropped
        uint32_t const cooked = kos_counter_load(&st.tx_bytes);
        TAP_CHECK(cooked > 0u and cooked <= sizeof(rbuf));
        // The return is INPUT bytes and the counter is COOKED bytes, so these differ under
        // KICKOS_CONSOLE_CRLF. Asserting equality here would pass on the sim, the ONLY
        // crlf=0 tree (the root CMakeLists sets crlf=1 for every arch that is not sim),
        // and fail on every other board.
        TAP_CHECK(cooked >= nb);
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // A bounded POSITIVE edge. Both helpers report whether the state was REACHED and never
    // that it was not: an absence has no event to wait on, so no arm may read a false return
    // as a verdict about ordering.
    constexpr uint64_t IRQ_EDGE_BUDGET_NS = 2000000000ull;
    constexpr uint64_t IRQ_EDGE_POLL_NS = 100000ull;

    // Above one kernel core a device line is targeted at core zero alone, so its handler runs
    // on a core of its own and the count main reads on the next instruction may not have
    // moved yet.
    bool irq_spurious_await(uint32_t target)
    {
        uint64_t const deadline = kos_clock_now() + IRQ_EDGE_BUDGET_NS;
        while (kos_irq_spurious_count() != target)
        {
            if (kos_clock_now() > deadline)
            {
                return false;
            }
            kos_sleep_ns(IRQ_EDGE_POLL_NS);
        }
        return true;
    }

    // A retired line comes back with its publication record's grace period, which ends when
    // every peer core has left the interrupt dispatch entry. That is not a property of the
    // calling thread's progress, so above one kernel core a first attempt can legally find
    // the line still retiring; only a refusal that never lifts is a lost line.
    int irq_claim_await(int line, kos_cap_t* out)
    {
        uint64_t const deadline = kos_clock_now() + IRQ_EDGE_BUDGET_NS;
        int rc = kos_irq_claim(line, KOS_IRQ_EDGE, out);
        while (rc == -KOS_EAGAIN)
        {
            if (kos_clock_now() > deadline)
            {
                return rc;
            }
            kos_sleep_ns(IRQ_EDGE_POLL_NS);
            rc = kos_irq_claim(line, KOS_IRQ_EDGE, out);
        }
        return rc;
    }

    // --- One driver per line: a second claim on a bound line is refused --------
    void t_irq_ownership()
    {
        constexpr int LINE = KICKOS_IRQ_FREE_BASE + 5;
        kos_cap_t owned = KOS_CAP_NONE;
        TAP_CHECK(irq_claim_await(LINE, &owned) == 0);
        // main has AUTH_IRQ, so this tests an occupied line rather than missing authority.
        kos_cap_t stolen = KOS_CAP_NONE;
        TAP_CHECK(kos_irq_claim(LINE, KOS_IRQ_EDGE, &stolen) == -KOS_EBUSY
                  and stolen == KOS_CAP_NONE);
        kos_handle_close(owned); // release the last reference
    }

    // --- The tier-1 mint is gated on KOS_AUTH_IRQ ------------------------------
    // The refusal MUST be witnessed from a worker, not from main: the suite declares
    // KOS_AUTH_IRQ, so a main-side claim tests the GRANT and can never see the refusal.
    // Keep this to ONE new static: on a 16 KiB part .bss added here shrinks the arena for
    // every later arm.
    int g_claimgate_rc = 0;
    constexpr int CLAIM_GATE_LINE = KICKOS_IRQ_FREE_BASE + 7;

    void claimgate_worker(void*) // UNPRIVILEGED, authority 0; caps: g_done@1 (CH_DONE)
    {
        kos_cap_t line = KOS_CAP_NONE;
        g_claimgate_rc = kos_irq_claim(CLAIM_GATE_LINE, KOS_IRQ_EDGE, &line);
        kos_sem_post(CH_DONE);
    }
    void t_irq_claim_gate()
    {
        g_claimgate_rc = 0;
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        // Above main, so it runs to completion including its exit before main is scheduled
        // again: its slot is then exited and reusable by the next arm's worker.
        auto w = kos::thread::create_caps(claimgate_worker, nullptr, "claimgate", 15, caps, 1);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        // EPERM, not EBUSY: the line is free, so only the authority gate can refuse it.
        TAP_CHECK(g_claimgate_rc == -KOS_EPERM);
        // The refusal left NOTHING behind: main can still claim that same line.
        kos_cap_t owned = KOS_CAP_NONE;
        TAP_CHECK(kos_irq_claim(CLAIM_GATE_LINE, KOS_IRQ_EDGE, &owned) == 0);
        TAP_CHECK(kos_handle_close(owned) == 0);
    }

    // --- A line comes back when its holder dies --------------------------------
    // A dying thread's line cap is dropped and the binding slot freed, so the SAME line is
    // claimable again. Without that release the line returns -KOS_EBUSY forever.
    constexpr int RECLAIM_LINE = KICKOS_IRQ_FREE_BASE + 8;

    // Do NOT rewrite this as wait-then-inject: a claim leaves the line masked and spawn
    // does not preempt, so main's inject lands masked, the worker's own first arm discards
    // the latch, and the arm deadlocks instead of testing. The ack reaches the ARMED state
    // with no event needed.
    void reclaim_worker(void*) // holds the delegated line cap at CH_IRQ, then EXITS
    {
        kos_irq_ack(CH_IRQ);
        kos_sem_post(CH_DONE);
    }
    // The object the reclaim line signals, held by main for the arm's length: the line must
    // signal SOMEWHERE before the worker's ack can arm it.
    kos_cap_t g_reclaim_note = KOS_CAP_NONE;
    void t_irq_reclaim()
    {
        kos_cap_t first = KOS_CAP_NONE;
        TAP_CHECK(kos_irq_claim(RECLAIM_LINE, KOS_IRQ_EDGE, &first) == 0);
        g_reclaim_note = notify_for_line(first);
        TAP_CHECK(g_reclaim_note != KOS_CAP_NONE);
        // done@1, line@3, index 2 deliberately EMPTY.
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {first, KOS_CAP_WAIT}};
        uint16_t const dest[] = {CH_DONE, CH_IRQ};
        auto w = irq_spawn(reclaim_worker, nullptr, "reclaim", 15, caps, 2,
                           KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0,
                           /*authority=*/0, dest);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            kos_handle_close(first);
            kos_handle_close(g_reclaim_note);
            return;
        }
        // main drops its copy BEFORE the worker dies, so the worker's exit is what takes the
        // refcount to zero. Closing after would not prove that DEATH releases the line. The
        // NOTIFICATION's own name goes too: the line's attachment holds a reference of its
        // own, so a copy left open here would keep the object alive past the line.
        TAP_CHECK(kos_handle_close(first) == 0);
        TAP_CHECK(kos_handle_close(g_reclaim_note) == 0);
        wait_n(1); // consumes the worker's post; the ORDER below comes from the join
        // The claim is that DEATH releases the line, so the edge has to be the death itself:
        // sched::exit_current runs cap_teardown BEFORE it wakes a joiner, so a join that
        // returns 0 has the worker's line cap already dropped. The post above is raised
        // before the worker returns and names no such point.
        int const jrc = w.join();
        kos_cap_t second = KOS_CAP_NONE;
        int const crc = irq_claim_await(RECLAIM_LINE, &second);
        int close_rc = 0;
        if (crc == 0)
        {
            close_rc = kos_handle_close(second);
        }
        g_reclaim_note = KOS_CAP_NONE;
        TAP_CHECK(jrc == 0);
        TAP_CHECK(crc == 0); // a refusal that never lifts means death did not release the line
        TAP_CHECK(close_rc == 0);
    }

    // Notification handover across a death: the first server binds, takes a raise it never
    // consumes, and dies. The pending bits live in the object, not the TCB, so the
    // successor's bind finds them. main's capability keeps the object and the line alive
    // across the gap.
    constexpr int HANDOVER_LINE = KICKOS_IRQ_FREE_BASE + 8;
    constexpr uint32_t HANDOVER_WAIT_US = 200000;
    Atomic<int32_t, Order::RELAXED> g_ho_first{-99}; // event inherited from the first server
    Atomic<int32_t, Order::RELAXED> g_ho_second{-99}; // event after rearming
    Atomic<uint32_t, Order::RELAXED> g_ho_bound{0};
    kos_cap_t g_ho_release = KOS_CAP_NONE;

    // caps: done@1, ready@2, note@3, release@4. The LINE is not delegated: the successor's
    // own wait is what rearms it, which is the second half of this arm.
    void ho_leaver(void*)
    {
        g_ho_bound = static_cast<uint32_t>(kos_notify_bind(CH_IRQ) == 0);
        kos_sem_post(CH_READY); // bound; main may inject
        kos_sem_wait(CH_REL);   // exit with the raise unconsumed and the binding still held
    }
    void ho_successor(void*)
    {
        (void)kos_notify_bind(CH_IRQ);
        uint32_t bits = 0;
        // main injects nothing between binds, so this must be the pending raise the leaver
        // left behind.
        g_ho_first = kos_notify_wait(CH_IRQ, NOTE_ALL, HANDOVER_WAIT_US, &bits);
        kos_sem_post(CH_READY); // main may inject the second event
        g_ho_second = kos_notify_wait(CH_IRQ, NOTE_ALL, HANDOVER_WAIT_US, &bits);
        kos_sem_post(CH_DONE);
    }
    void t_irq_server_handover()
    {
        g_ho_first = -99;
        g_ho_second = -99;
        g_ho_bound = 0;
        kos_cap_t line = KOS_CAP_NONE;
        TAP_CHECK(irq_claim_await(HANDOVER_LINE, &line) == 0);
        kos_cap_t note = notify_for_line(line);
        TAP_CHECK(note != KOS_CAP_NONE);
        // Arm the claimed line before injection.
        TAP_CHECK(kos_irq_ack(line) == 0);
        kos_sem_create(0, &g_irqdrv_done);
        kos_sem_create(0, &g_irq_ready);
        kos_sem_create(0, &g_ho_release);
        kos_cap_grant caps[] = {{g_irqdrv_done, CH_FULL}, {g_irq_ready, CH_FULL},
                                {note, CH_FULL}, {g_ho_release, CH_FULL}};
        auto first = irq_spawn(ho_leaver, nullptr, "hoL", 15, caps, 4);
        if (not first.valid())
        {
            kos_sem_destroy(g_irqdrv_done);
            kos_sem_destroy(g_irq_ready);
            kos_sem_destroy(g_ho_release);
            kos_handle_close(line);
            kos_handle_close(note);
            tap::skip("thread pool too small");
            return;
        }
        kos_sem_wait(g_irq_ready); // first server is bound
        kos_irq_inject(HANDOVER_LINE);
        kos_sem_post(g_ho_release); // exit with the event pending
        int const jrc = first.join();
        auto second = irq_spawn(ho_successor, nullptr, "hoS", 15, caps, 4);
        if (not second.valid())
        {
            kos_sem_destroy(g_irqdrv_done);
            kos_sem_destroy(g_irq_ready);
            kos_sem_destroy(g_ho_release);
            kos_handle_close(line);
            kos_handle_close(note);
            tap::skip("thread pool too small");
            return;
        }
        kos_sem_wait(g_irq_ready); // successor finished its first wait
        kos_irq_inject(HANDOVER_LINE);
        kos_sem_wait(g_irqdrv_done);
        kos_sem_destroy(g_irqdrv_done);
        kos_sem_destroy(g_irq_ready);
        kos_sem_destroy(g_ho_release);
        TAP_CHECK(kos_handle_close(line) == 0);
        TAP_CHECK(kos_handle_close(note) == 0);
        tap::diag("handover: bound %u, inherited %d, next raise %d",
                  static_cast<unsigned>(g_ho_bound.load()),
                  static_cast<int>(g_ho_first.load()),
                  static_cast<int>(g_ho_second.load()));
        TAP_CHECK(g_ho_bound.load() == 1u);
        TAP_CHECK(jrc == 0);
        TAP_CHECK(g_ho_first.load() == 0);
        TAP_CHECK(g_ho_second.load() == 0);
    }

    // Two software raises of one badge coalesce into one bit; the second returns EALREADY,
    // and a DRAINED repost answers 0 again. main binds and waits on its own object, avoiding
    // cross-thread ordering. The raise goes through a BADGED copy, which is the only kind a
    // confined signaller ever holds.
    void t_irq_notify_already()
    {
        constexpr uint32_t DOORBELL_BIT = 5;
        kos_cap_t note = KOS_CAP_NONE;
        TAP_CHECK(kos_notify_create(&note) == 0);
        kos_cap_t bell = KOS_CAP_NONE;
        TAP_CHECK(kos_notify_badge(note, DOORBELL_BIT, &bell) == 0);
        TAP_CHECK(kos_notify_bind(note) == 0);
        uint32_t bits = 0;
        TAP_CHECK(kos_notify(bell) == 0);
        TAP_CHECK(kos_notify(bell) == -KOS_EALREADY);
        // One wait consumes both coalesced raises without parking, and names the bit.
        TAP_CHECK(kos_notify_wait(note, NOTE_ALL, 0u, &bits) == 0
                  and bits == (1u << DOORBELL_BIT));
        // After consumption, a new raise must succeed again.
        TAP_CHECK(kos_notify(bell) == 0);
        bits = 0;
        TAP_CHECK(kos_notify_wait(note, NOTE_ALL, 0u, &bits) == 0
                  and bits == (1u << DOORBELL_BIT));
        TAP_CHECK(kos_notify_unbind(note) == 0);
        TAP_CHECK(kos_handle_close(bell) == 0);
        TAP_CHECK(kos_handle_close(note) == 0);
    }

    // --- Spurious IRQ: an unbound line is masked + counted, never dropped -------
    void t_irq_spurious()
    {
        constexpr int FREE_LINE = KICKOS_IRQ_FREE_BASE + 3; // no driver bound to this line
        // Enable the line so the injected raise reaches the default handler: ARM NVIC and RX
        // are masked by default, sim/riscv are not.
        kos_irq_unmask(FREE_LINE);
        uint32_t const before = kos_irq_spurious_count();
        kos_irq_inject(FREE_LINE);   // default handler runs: mask + bump counter
        TAP_CHECK(irq_spurious_await(before + 1));
#if KICKOS_KERNEL_CORES > 1
        // The second half is not witnessed above one kernel core, and no deadline fixes it: a
        // non-delivery raises no event, so there is no closed interval to read the counter
        // in. Checked on every one-core preset, where the inject and its handler run on the
        // calling core.
        tap::partial("a masked line's non-delivery has no event to be ordered after");
#else
        // The default handler masked the line, so a second raise LATCHES rather than
        // delivering: the counter must NOT advance until the line is unmasked again.
        kos_irq_inject(FREE_LINE);
        TAP_CHECK(kos_irq_spurious_count() == before + 1);
#endif
    }

    // --- First-arm discards pre-claim garbage ----------------------------------
    // A raise that lands before a driver owns the line is latched, and the first arm must
    // discard that stale latch (arch_irq_clear_pending) or the first irq.wait() phantom-
    // wakes on garbage. This is the one tier-1 arm where main must not pre-arm the line:
    // pre-arming moves the discard into main and stops testing the driver's own first arm.
    int g_stale_seen = 0;
    constexpr int STALE_LINE = KICKOS_IRQ_FREE_BASE + 6;

    void stale_driver(void*)
    {
        auto note = kos::Notification::adopt(CH_NOTE);
        note.bind();
        kos_sem_post(CH_READY); // g_irq_ready
        note.wait(NOTE_ALL);    // first arm discards the latch, then MUST block
        g_stale_seen++;                           // only after main injects a REAL event
        kos_sem_post(CH_DONE);
    }
    void t_irq_stale_register()
    {
        kos_sem_create(0, &g_irq_ready);
        g_stale_seen = 0;
        // Pre-registration garbage: unmask so the default handler runs (mask + count) on the
        // first raise; the second raise then latches on the now-masked line.
        kos_irq_unmask(STALE_LINE);
        uint32_t const before = kos_irq_spurious_count();
        kos_irq_inject(STALE_LINE);
        if (not irq_spurious_await(before + 1))
        {
            kos_sem_destroy(g_irq_ready);
            tap::fail("the pre-registration raise never reached the default handler");
            return;
        }
        kos_irq_inject(STALE_LINE);
        // Claim but deliberately do NOT arm: the latch must still be there for the
        // driver's own first wait to discard.
        kos_cap_t irq = KOS_CAP_NONE;
        if (kos_irq_claim(STALE_LINE, KOS_IRQ_EDGE, &irq) != 0)
        {
            kos_sem_destroy(g_irq_ready);
            tap::fail("the stale line could not be claimed");
            return;
        }
        // Attaching does NOT arm: the latch stays for the driver's own first wait to discard.
        kos_cap_t note = notify_for_line(irq);
        if (note == KOS_CAP_NONE)
        {
            kos_handle_close(irq);
            kos_sem_destroy(g_irq_ready);
            tap::fail("the stale line could not be attached to a notification");
            return;
        }
        kos_cap_grant caps[] = {{g_done, CH_FULL},
                                {g_irq_ready, CH_FULL},
                                {irq, KOS_CAP_WAIT},
                                {note, CH_FULL}};
        auto drv = irq_spawn(stale_driver, nullptr, "staleirq", 1, caps, 4);
        if (not drv.valid())
        {
            kos_handle_close(irq);
            kos_handle_close(note);
            kos_sem_destroy(g_irq_ready);
            tap::fail("the stale driver did not spawn"); // its absence would hang the wait below
            return;
        }
        kos_handle_close(irq);
        kos_handle_close(note);
        kos_sem_wait(g_irq_ready); // driver is bound, about to take its first wait
        kos_sem_destroy(g_irq_ready);
        bool no_phantom = true;
#if KICKOS_KERNEL_CORES > 1
        // The no-phantom half is not witnessed above one kernel core. A wake that must not
        // happen raises no event, and the liveness half below cannot separate the two: under
        // the phantom the driver wakes on the stale latch, and main's later inject then lands
        // on a line with no waiter, leaving the same count. Checked on every one-core preset.
        tap::partial("a phantom wake is a wake that must NOT happen");
#else
        kos_sleep_ns(2000000ull);
        no_phantom = (g_stale_seen == 0);
#endif
        // Liveness, on every core count. main cannot observe the driver REACH its first
        // wait, and that wait is what arms the line: a raise landing before it is cleared by
        // the arm itself, which is the behaviour under test. So raise until one is delivered,
        // bounded. This leaves stray raises on STALE_LINE: every raise after the driver has
        // serviced reaches the null-object default, which masks the line and bumps the
        // spurious count, so an arm that reads that count after this one must take its own
        // `before` reading rather than one taken earlier.
        uint64_t const deadline = kos_clock_now() + IRQ_EDGE_BUDGET_NS;
        kos_irq_inject(STALE_LINE);
        while (g_stale_seen == 0 and kos_clock_now() <= deadline)
        {
            kos_sleep_ns(IRQ_EDGE_POLL_NS);
            kos_irq_inject(STALE_LINE);
        }
        bool const live = (g_stale_seen == 1);
        if (live)
        {
            wait_n(1); // the driver's own post, so the counter is left balanced
        }

        TAP_CHECK(no_phantom);
        TAP_CHECK(live);
    }
#endif

    // --- Caller-owned thread stack: spawn takes a caller-provided stack, and rejects an
    // undersized or misaligned one -------------------------------------------------------
    kos_cap_t g_cstk_sem = KOS_CAP_NONE;
    void caller_stack_worker(void*) { kos_sem_post(CH_DONE); } // g_cstk_sem at CH_DONE

    void cstk_wait_if_run(kos::thread::Handle const& t)
    {
        if (t.valid())
        {
            kos_sem_wait(g_cstk_sem);
        }
    }

    // The least a spawn charges a caller's stack above KICKOS_MIN_STACK_SIZE: under
    // KICKOS_REENT_IN_TCB the TLS control block, one KICKOS_STACK_ALIGN unit, is always carved.
#if KICKOS_REENT_IN_TCB
    constexpr uint32_t CSTK_CARVE_LEAST = 16u;
#else
    constexpr uint32_t CSTK_CARVE_LEAST = 0u;
#endif

    // A static lands outside the arena, which an enforcing backend confines a stack to.
#if !KICKOS_MEMORY_ENFORCED
#if defined(KICKOS_TLS) && KICKOS_TLS
    constexpr uint32_t CSTK_BLOCK = KICKOS_TLS_STRIDE;
#else
    constexpr uint32_t CSTK_BLOCK = KICKOS_MIN_STACK_SIZE + 48u;
#endif
    // Neither a power of two nor stride-aligned, and 16 past a 32-byte boundary: with no region
    // descriptor and no thread_local declared, spawn owes it the ABI's alignment and nothing
    // more on every arch. The worker runs the deepest kernel dispatch on it, hence the floor.
    constexpr uint32_t CSTK_ODD = CSTK_BLOCK - 32u;
    static_assert(CSTK_ODD >= KICKOS_MIN_STACK_SIZE + CSTK_CARVE_LEAST,
                  "below the per-arch stack floor");
    static_assert((CSTK_ODD & (CSTK_ODD - 1u)) != 0u, "a power of two proves nothing here");
    alignas(32) unsigned char g_cstk_block[CSTK_BLOCK];
    unsigned char* const g_cstk_odd = g_cstk_block + 16u;
#if KICKOS_LIBC_REENT
    int g_cstk_odd_errno = 0;
    // caps: g_cstk_sem at CH_DONE, the release at CH_READY
    void caller_stack_errno_worker(void*)
    {
        char* end = nullptr;
        (void)strtol("10", &end, 99);
        kos_sem_post(CH_DONE);
        kos_sem_wait(CH_READY);
        g_cstk_odd_errno = errno;
        kos_sem_post(CH_DONE);
    }
#endif
#endif
    // A power of two, doubled until it is past the spawn floor, which the TLS carve adds to.
    constexpr uint32_t cstk_size()
    {
        uint32_t size = 2048;
        while (size <= KICKOS_MIN_STACK_SIZE)
        {
            size = size * 2u;
        }
        return size;
    }

    void t_caller_stack()
    {
        // Reject a non-null, tiny + misaligned caller stack: -KOS_EINVAL, not run or corrupt.
        TAP_CHECK(kos::thread::create(caller_stack_worker, nullptr, "badstk", 10, KOS_POLICY_FIFO,
                                      0, false, nullptr, 0, reinterpret_cast<void*>(0x1), 8).error()
                  == -KOS_EINVAL);
        // Accept a properly-sized, aligned caller-owned stack. When the arena cannot spare
        // one the reject case above has already run, so the arm stays `ok` as a partial.
        constexpr uint32_t STK = cstk_size();
        // Where regions are powers of two the block is aligned to its own size, and 16 bytes
        // past STK would double it. Elsewhere the 16 stand: the smallest parts' arena
        // measurements were taken with them.
#if defined(KICKOS_MPU_MIN_REGION_CFG) and defined(KICKOS_MPU_REGION_POW2_CFG) \
    and KICKOS_MPU_MIN_REGION_CFG != 0 and KICKOS_MPU_REGION_POW2_CFG != 0
        constexpr uint32_t CSTK_RESERVE = STK;
#else
        constexpr uint32_t CSTK_RESERVE = STK + 16u;
#endif
        void* raw = kos_ram_alloc(CSTK_RESERVE);
        if (raw == nullptr)
        {
            tap::partial("accept half not run (arena cannot spare a stack)");
            return;
        }
        // Allocation grants nothing, and where a backend translates the block is not even
        // mapped: a child started on it would fault on its first push. A region backend
        // seats the child's stack descriptor itself, and a grant there would hold one of
        // main's own descriptors for the life of the run.
#if KICKOS_HAVE_ASPACE
        TAP_CHECK(kos_mem_self_grant(raw, CSTK_RESERVE, 0) == 0);
#endif
        void* stk = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(raw) + 15u) & ~uintptr_t{15});
        // One KICKOS_STACK_ALIGN unit below the least floor a spawn charges, with an aligned
        // base, so only the size check can reject it.
        TAP_CHECK(kos::thread::create(caller_stack_worker, nullptr, "undf", 10, KOS_POLICY_FIFO,
                                      0, false, nullptr, 0, stk,
                                      KICKOS_MIN_STACK_SIZE + CSTK_CARVE_LEAST - 16u, nullptr,
                                      0, nullptr, 0).error()
                  == -KOS_EINVAL);
        kos_sem_create(0, &g_cstk_sem);
        kos_cap_grant caps[] = {{g_cstk_sem, CH_FULL}};
        auto const t = kos::thread::create(caller_stack_worker, nullptr, "cstk", 10, KOS_POLICY_FIFO,
                                           0, false, nullptr, 0, stk, STK, nullptr, 0, caps, 1);
        TAP_CHECK(t.valid());
        cstk_wait_if_run(t);
        kos_sem_destroy(g_cstk_sem);
#if !KICKOS_MEMORY_ENFORCED
#if KICKOS_LIBC_REENT
        // The worker sets its errno through libc, parks, and reads it back after this thread has
        // set a different one: a thread whose libc state was never seated shares this one's.
        kos_cap_t release = KOS_CAP_NONE;
        kos_sem_create(0, &g_cstk_sem);
        kos_sem_create(0, &release);
        g_cstk_odd_errno = 0;
        kos_cap_grant ocaps[] = {{g_cstk_sem, CH_FULL}, {release, CH_FULL}};
        auto const to = kos::thread::create(caller_stack_errno_worker, nullptr, "cstkO", 10,
                                            KOS_POLICY_FIFO, 0, false, nullptr, 0, g_cstk_odd,
                                            CSTK_ODD, nullptr, 0, ocaps, 2);
        TAP_CHECK(to.valid());
        if (to.valid())
        {
            kos_sem_wait(g_cstk_sem);
            char* end = nullptr;
            (void)strtol("99999999999999999999999999", &end, 10);
            int const mine = errno;
            kos_sem_post(release);
            kos_sem_wait(g_cstk_sem);
            TAP_CHECK(mine == ERANGE);
            TAP_CHECK(g_cstk_odd_errno == EINVAL);
            TAP_CHECK(errno == ERANGE);
        }
        kos_sem_destroy(release);
        kos_sem_destroy(g_cstk_sem);
#else
        kos_sem_create(0, &g_cstk_sem);
        kos_cap_grant ocaps[] = {{g_cstk_sem, CH_FULL}};
        auto const to = kos::thread::create(caller_stack_worker, nullptr, "cstkO", 10,
                                            KOS_POLICY_FIFO, 0, false, nullptr, 0, g_cstk_odd,
                                            CSTK_ODD, nullptr, 0, ocaps, 1);
        TAP_CHECK(to.valid());
        cstk_wait_if_run(to);
        kos_sem_destroy(g_cstk_sem);
#endif
#endif
    }

    // --- Caller-owned stack outside the arena ------------------------------------
    // A KOS_STACK_DEFINE stack lands in app .bss, outside the user-RAM arena, and an
    // unprivileged child's stack is an arena-confined grant: -KOS_EPERM, and no other code.
    // Sized AND aligned to the TLS stride so the seat would admit it and only the arena
    // bound can refuse it. Translating backends only: a stride-aligned static does not fit
    // every .appdata window (esp32c6 carves 4K).
#if KICKOS_HAVE_ASPACE
#if defined(KICKOS_TLS) && KICKOS_TLS
    alignas(KICKOS_TLS_STRIDE) unsigned char g_cstk_outside[KICKOS_TLS_STRIDE];
#else
    KOS_STACK_DEFINE(g_cstk_outside, KICKOS_MIN_STACK_SIZE);
#endif
    void t_caller_stack_arena()
    {
        TAP_CHECK(kos::thread::create(caller_stack_worker, nullptr, "cstkO", 10, KOS_POLICY_FIFO,
                                      0, false, nullptr, 0, g_cstk_outside,
                                      static_cast<uint32_t>(sizeof(g_cstk_outside))).error()
                  == -KOS_EPERM);
    }
#endif

    // --- A self-granted range shared by three threads of one task --------------------
    // The two workers are plain spawns, so they are threads of main's task and share the
    // four globals below with it. Each ASKS for the range itself, which is the portable
    // floor: a grant guarantees access to its HOLDER and says nothing about a peer.
    volatile int* g_dshared = nullptr;
    kos_cap_t g_dwrote = KOS_CAP_NONE; // writer -> reader handoff (through the handed-over range)
    kos_cap_t g_dread = KOS_CAP_NONE;  // reader -> main handoff
    int g_dreadback = -1;
    constexpr int DOM_SENTINEL = 0x5A5A;
    void dom_writer(void*) // caps: g_dwrote@1 (CH_DONE)
    {
        (void)kos_mem_self_grant(const_cast<int*>(g_dshared), 256, 0);
        *g_dshared = DOM_SENTINEL;
        kos_sem_post(CH_DONE);     // g_dwrote
    }
    void dom_reader(void*) // caps: g_dwrote@1 (CH_DONE), g_dread@2 (CH_READY)
    {
        (void)kos_mem_self_grant(const_cast<int*>(g_dshared), 256, 0);
        kos_sem_wait(CH_DONE);    // g_dwrote: after the writer stored the sentinel
        g_dreadback = *g_dshared;
        kos_sem_post(CH_READY);   // g_dread
    }
    void t_domain_share()
    {
        // Alloc before the sems so an early return leaks nothing.
        g_dshared = static_cast<volatile int*>(kos_ram_alloc(256));
        if (g_dshared == nullptr)
        {
            tap::skip("arena cannot spare the shared region");
            return;
        }
        // main's own reach.
        TAP_CHECK(kos_mem_self_grant(const_cast<int*>(g_dshared), 256, 0) == 0);
        *g_dshared = 0;
        g_dreadback = -1;
        kos_sem_create(0, &g_dwrote);
        kos_sem_create(0, &g_dread);
        // Spawn BOTH before either runs (spawn does not preempt).
        kos_cap_grant wcaps[] = {{g_dwrote, CH_FULL}};
        kos_cap_grant rcaps[] = {{g_dwrote, CH_FULL}, {g_dread, CH_FULL}};
        auto w = kos::thread::create_caps(dom_writer, nullptr, "domW", 10, wcaps, 1,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                          KOS_AUTH_MEMORY);
        auto r = kos::thread::create_caps(dom_reader, nullptr, "domR", 10, rcaps, 2,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                          KOS_AUTH_MEMORY);
        if (not w.valid() or not r.valid())
        {
            // Whichever worker did spawn self-completes, so nothing needs draining.
            tap::skip("thread pool too small for 2 concurrent");
            kos_sem_destroy(g_dwrote);
            kos_sem_destroy(g_dread);
            return;
        }
        kos_sem_wait(g_dread); // the reader saw the writer's store via the shared region
        kos_sem_destroy(g_dwrote);
        kos_sem_destroy(g_dread);
        TAP_CHECK(g_dreadback == DOM_SENTINEL);
    }

#if KICKOS_MEMORY_ENFORCED
    // One task cannot use another's reservation through self-grant, spawn data, or a
    // caller-supplied stack: -KOS_EPERM on MMU and MPU backends alike. The worker's grant of
    // its own reservation excludes missing authority as the cause.
    enum
    {
        XG_OWN = 0,      // the worker's own reservation: granted
        XG_FOREIGN = 1,  // main's reservation, self-granted from another task: refused
        XG_MEMBASE = 2,  // main's reservation as a child's domain region: refused
        XG_STACK = 3,    // main's reservation as a child's stack: refused
        XG_WORDS = 4
    };
    constexpr uint32_t XG_BLK = 64;
    // main's block doubles as the stack the third case names, so it must clear the spawn's
    // own size and alignment gates and leave ownership the only thing that can refuse it.
#if defined(KICKOS_TLS) && KICKOS_TLS
    constexpr uint32_t XG_STK = KICKOS_TLS_STRIDE;
#else
    constexpr uint32_t XG_STK = KICKOS_MIN_STACK_SIZE;
#endif
    void xg_noop(void*) {}
    void xg_worker(void* arg) // caps: done@1, E(SIGNAL)@2
    {
        int32_t rep[XG_WORDS] = {1, 1, 1, 1};
        void* const mine = kos_ram_alloc(XG_BLK);
        if (mine != nullptr)
        {
            rep[XG_OWN] = kos_mem_self_grant(mine, XG_BLK, 0);
        }
        // main's address, carried as a NUMBER and never dereferenced: this task does not
        // reach it, and the point is that it cannot make it reach it.
        rep[XG_FOREIGN] = kos_mem_self_grant(arg, XG_BLK, 0);
        rep[XG_MEMBASE] = kos::thread::create(xg_noop, nullptr, "xgmem", 10, KOS_POLICY_FIFO,
                                              0, false, arg, XG_STK).error();
        rep[XG_STACK] = kos::thread::create(xg_noop, nullptr, "xgstk", 10, KOS_POLICY_FIFO,
                                            0, false, nullptr, 0, arg, XG_STK).error();
        (void)kos_send(2, rep, sizeof(rep));
        kos_sem_post(CH_DONE);
    }
    void t_cross_task_block()
    {
        void* const theirs = kos_ram_alloc(XG_STK);
        if (theirs == nullptr)
        {
            tap::skip("arena cannot spare the donor block");
            return;
        }
        // main maps it, so the worker names a range that really is live somewhere.
        TAP_CHECK(kos_mem_self_grant(theirs, XG_STK, 0) == 0);
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            tap::skip("endpoint pool too small");
            return;
        }
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            (void)kos_handle_close(ep);
            tap::skip("task pool too small");
            return;
        }
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {ep, KOS_CAP_SIGNAL}};
        if (not kos::thread::create_caps(xg_worker, theirs, "xgrnt", 10, caps, 2,
                                         KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                         KOS_AUTH_MEMORY, nullptr, t).valid())
        {
            (void)kos_task_kill(t);
            (void)kos_handle_close(ep);
            tap::skip("thread pool too small");
            return;
        }
        int32_t rep[XG_WORDS] = {1, 1, 1, 1};
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, ep, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        bool const heard =
            kos_reply_recv(KOS_CAP_NONE, rep, kos_call_lens_pack(0, sizeof(rep)), &o)
            == static_cast<int32_t>(sizeof(rep));
        wait_n(1);
        (void)kos_task_kill(t);
        (void)kos_handle_close(ep);
        TAP_CHECK(heard);
        TAP_CHECK(rep[XG_OWN] == 0);
        TAP_CHECK(rep[XG_FOREIGN] == -KOS_EPERM);
        TAP_CHECK(rep[XG_MEMBASE] == -KOS_EPERM);
        TAP_CHECK(rep[XG_STACK] == -KOS_EPERM);
    }
#endif

    // A one-entry device window list, alive until the end of the spawn expression naming it.
    struct DeviceWindow
    {
        kos_window w;
        DeviceWindow(uintptr_t base, uint32_t size) : w{base, size, KOS_WINDOW_DEVICE, 0} {}
        kos_window const* list() const
        {
            return &w;
        }
    };

#if KICKOS_MEMORY_ENFORCED && KICKOS_FAULT_ISOLATION
    // --- A read-only memory window ------------------------------------------------------
    // Children name main's block as a memory window, main itself never reaching it, each at
    // the address kos_window_get answers: the block's own on a region board, one the kernel
    // chose on a translating one. The first writes a byte through a read-write window; the
    // second, in a task of its own, reads it through a read-only one and faults on its write,
    // which ends that task and nothing else; on a region board the third holds it both ways and
    // has the kernel refuse a write into it; the last reads back through a read-write window
    // what the faulted and the refused writes left.
    constexpr unsigned char WRO_BYTE = 0x5A;
    constexpr uint32_t WRO_JOIN_US = 500000;
    // What a child reports before it writes: a child in a task of its own holds its own copy of
    // the app's data where a space is per task, so the report travels over an endpoint.
    struct WroSeen
    {
        int32_t read;
        int32_t moved; // the window's address differs from the block's
    };
    Atomic<int32_t, Order::RELAXED> g_wro_put{-1}; // what the child writes, or -1 for none
    Atomic<int32_t, Order::RELAXED> g_wro_wrote{0}; // seen by main for a child of its own task
    void wro_child(void* arg) // caps: E(SIGNAL)@1
    {
        kos_window w = {};
        void* at = nullptr;
        WroSeen seen = {-1, -1};
        if (kos_window_get(0, &w) == 0)
        {
            at = reinterpret_cast<void*>(w.base);
            seen.moved = 0;
            if (at != arg)
            {
                seen.moved = 1;
            }
            seen.read = *static_cast<volatile unsigned char*>(at);
        }
        (void)kos_send(1, &seen, sizeof(seen));
        if (seen.read >= 0 and g_wro_put >= 0)
        {
            *static_cast<volatile unsigned char*>(at) = static_cast<unsigned char>(g_wro_put.load());
            g_wro_wrote = 1;
        }
        kos_exit(0);
    }
    // Parks holding its window until main posts `hold`.
    void wro_hold_child(void*) // caps: hold@1
    {
        kos_sem_wait(1);
        kos_exit(0);
    }
#if not KICKOS_HAVE_ASPACE
    // Holds the block as its own task's data region and through a read-only window, and names
    // it as a call's reply buffer, which the kernel checks for writing before it looks at the
    // capability: the read-only window must win there too. It never touches the block itself.
    Atomic<int32_t, Order::RELAXED> g_wro_sys{-1};
    void wro_sys_child(void* arg)
    {
        g_wro_sys = kos_call_timed(1, arg, 0, sizeof(uint32_t), 1000);
        kos_exit(0);
    }
#endif
    // One child through one window, reporting on `ep`: 0 when it joined, and its report.
    int wro_run(void* blk, uint32_t size, uint8_t flags, int32_t put, kos_task_t task,
                kos_cap_t ep, WroSeen* seen)
    {
        g_wro_put = put;
        g_wro_wrote = 0;
        *seen = {-1, -1};
        kos_window const w = {reinterpret_cast<uintptr_t>(blk), size, KOS_WINDOW_MEMORY, flags};
        kos_cap_grant const caps[] = {{ep, KOS_CAP_SIGNAL}};
        auto const c = kos::thread::create(wro_child, blk, "wro", 10, KOS_POLICY_FIFO, 0, false,
                                           nullptr, 0, nullptr, 0, &w, 1, caps, 1, 0, nullptr,
                                           task);
        int rc = c.error();
        if (rc == 0)
        {
            struct kos_reply_recv_opts o;
            kos_reply_recv_opts_init(&o, ep, KOS_RECV_NO_INFO, WRO_JOIN_US);
            (void)kos_reply_recv(KOS_CAP_NONE, seen, kos_call_lens_pack(0, sizeof(*seen)), &o);
            rc = c.join(WRO_JOIN_US);
        }
        return rc;
    }
    void t_window_memory_ro()
    {
        size_t const g = discover_granule();
        void* blk = nullptr;
        if (g != 0)
        {
            blk = kos_ram_alloc(g);
        }
        kos_task_t t = KOS_TASK_NONE;
        kos_cap_t ep = KOS_CAP_NONE;
        if (blk == nullptr or kos_endpoint_create(&ep) != 0
            or kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            tap::skip("arena, endpoint or task pool too small");
            return;
        }
        uint32_t const size = static_cast<uint32_t>(g);
        WroSeen seen = {-1, -1};
        TAP_CHECK(wro_run(blk, size, 0, WRO_BYTE, KOS_TASK_NONE, ep, &seen) == 0);
        TAP_CHECK(g_wro_wrote == 1);
        TAP_CHECK(seen.moved == KICKOS_HAVE_ASPACE);
        TAP_CHECK(wro_run(blk, size, KOS_WINDOW_RO, 0, t, ep, &seen) == 0);
        TAP_CHECK(seen.read == WRO_BYTE);
        (void)kos_task_kill(t);
        // A list names a block once.
        kos_window const twice[] = {
            {reinterpret_cast<uintptr_t>(blk), size, KOS_WINDOW_MEMORY, 0},
            {reinterpret_cast<uintptr_t>(blk), size, KOS_WINDOW_MEMORY, KOS_WINDOW_RO}};
        TAP_CHECK(kos::thread::create(wro_hold_child, nullptr, "wrod", 10, KOS_POLICY_FIFO, 0,
                                      false, nullptr, 0, nullptr, 0, twice, 2).error()
                  == -KOS_EINVAL);
        // Held as the child's own data region and through no window, the block is no window:
        // kos_window_get answers the spawn list alone.
        kos_cap_grant const dcaps[] = {{ep, KOS_CAP_SIGNAL}};
        auto const data = kos::thread::create(wro_child, blk, "wroa", 10, KOS_POLICY_FIFO, 0,
                                              false, blk, size, nullptr, 0, nullptr, 0, dcaps, 1);
        seen = {-2, -2};
        if (data.valid())
        {
            struct kos_reply_recv_opts o;
            kos_reply_recv_opts_init(&o, ep, KOS_RECV_NO_INFO, WRO_JOIN_US);
            (void)kos_reply_recv(KOS_CAP_NONE, &seen, kos_call_lens_pack(0, sizeof(seen)), &o);
            (void)data.join(WRO_JOIN_US);
        }
        TAP_CHECK(data.valid() and seen.read == -1 and seen.moved == -1);
#if not KICKOS_HAVE_ASPACE
        kos_window const ro = {reinterpret_cast<uintptr_t>(blk), size, KOS_WINDOW_MEMORY,
                               KOS_WINDOW_RO};
        g_wro_sys = -1;
        auto const sys = kos::thread::create(wro_sys_child, blk, "wros", 10, KOS_POLICY_FIFO, 0,
                                             false, blk, size, nullptr, 0, &ro, 1);
        TAP_CHECK(sys.valid() and sys.join(WRO_JOIN_US) == 0);
        TAP_CHECK(g_wro_sys == -KOS_EFAULT);
#endif
        TAP_CHECK(wro_run(blk, size, 0, -1, KOS_TASK_NONE, ep, &seen) == 0);
        TAP_CHECK(seen.read == WRO_BYTE);
        (void)kos_handle_close(ep);
        // A block held uncached keeps that type: a cacheable window over it, and the owner's
        // own cacheable grant, are both refused while the uncached window lives.
        void* const other = kos_ram_alloc(g);
        kos_cap_t hold = KOS_CAP_NONE;
        if (other == nullptr or kos_sem_create(0, &hold) != 0)
        {
            tap::partial("type agreement not run (arena or semaphore pool)");
            return;
        }
        kos_window const nc = {reinterpret_cast<uintptr_t>(other), size, KOS_WINDOW_MEMORY,
                               KOS_WINDOW_UNCACHED};
        kos_cap_grant const hcaps[] = {{hold, KOS_CAP_WAIT}};
        auto const holder = kos::thread::create(wro_hold_child, nullptr, "wroh", 10,
                                                KOS_POLICY_FIFO, 0, false, nullptr, 0, nullptr,
                                                0, &nc, 1, hcaps, 1);
        if (holder.error() == -KOS_ENOTSUP)
        {
            (void)kos_handle_close(hold);
            tap::partial("type agreement not run (no uncached memory on this board)");
            return;
        }
        TAP_CHECK(holder.valid());
        kos_window const cached = {reinterpret_cast<uintptr_t>(other), size, KOS_WINDOW_MEMORY,
                                   0};
        TAP_CHECK(kos::thread::create(wro_hold_child, nullptr, "wroc", 10, KOS_POLICY_FIFO, 0,
                                      false, nullptr, 0, nullptr, 0, &cached, 1).error()
                  == -KOS_EBUSY);
        TAP_CHECK(kos_mem_self_grant(other, g, 0) == -KOS_EBUSY);
        // Nor may a new task take the block as its cacheable data.
        TAP_CHECK(kos::thread::create(wro_hold_child, nullptr, "wrom", 10, KOS_POLICY_FIFO, 0,
                                      false, other, size).error()
                  == -KOS_EBUSY);
        kos_sem_post(hold);
        TAP_CHECK(not holder.valid() or holder.join(WRO_JOIN_US) == 0);
        (void)kos_handle_close(hold);
        // One spawn asking for a block as cacheable task data and through an uncached window
        // is refused too, though neither mapping exists when its windows are admitted.
        void* const both = kos_ram_alloc(g);
        if (both == nullptr)
        {
            tap::partial("one-spawn type agreement not run (arena)");
            return;
        }
        kos_window const nc_both = {reinterpret_cast<uintptr_t>(both), size, KOS_WINDOW_MEMORY,
                                    KOS_WINDOW_UNCACHED};
        TAP_CHECK(kos::thread::create(wro_hold_child, nullptr, "wrob", 10, KOS_POLICY_FIFO, 0,
                                      false, both, size, nullptr, 0, &nc_both, 1).error()
                  == -KOS_EBUSY);
        // A task holds its data with no member yet: a second empty task taking the same block
        // with another memory type is refused as a spawn would be.
        void* const shared = kos_ram_alloc(g);
        kos_task_t nc_task = KOS_TASK_NONE;
        kos_task_t cached_task = KOS_TASK_NONE;
        int nrc = -KOS_ENOMEM;
        if (shared != nullptr)
        {
            nrc = kos_task_create(shared, size, KOS_MEM_NOCACHE, &nc_task);
        }
        if (nrc != 0)
        {
            tap::partial("empty-task type agreement not run (rc %d)", nrc);
            return;
        }
        TAP_CHECK(kos_task_create(shared, size, 0, &cached_task) == -KOS_EBUSY);
        (void)kos_task_kill(nc_task);
    }
#endif

    // --- MMIO grant boundary: privileged-only + encodable-only -------------------
    // The positive grant is HW-only, so this arm pins the two refusals: a window one MPU
    // descriptor cannot cover exactly, and any grant attempted by an UNPRIVILEGED caller.
    // The sim's arch_mpu_region_encodable admits ONE window (its fake register block) and
    // neither of these names it, so both halves still refuse there.
    int g_mmio_unpriv_rc = -2;
    kos_cap_t g_mmio_done = KOS_CAP_NONE;
    void mmio_noop(void*) {}
    void mmio_unpriv_worker(void*)
    {
        // Unprivileged caller: the privilege gate must refuse the MMIO grant.
        g_mmio_unpriv_rc = kos::thread::create(mmio_noop, nullptr, "mmiochild", 10,
                                               KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                               nullptr, 0,
                                               DeviceWindow(0x1000u, 4096).list(), 1)
                               .error();
        kos_sem_post(CH_DONE); // g_mmio_done
    }
    void t_mmio_grant()
    {
        // Non-encodable window (size 1, unaligned base): rejected -KOS_EINVAL, not rounded.
        // Geometry is checked ahead of the privilege gate, so the code holds in any posture.
        TAP_CHECK(kos::thread::create(mmio_noop, nullptr, "mmiobad", 10, KOS_POLICY_FIFO,
                                      0, false, nullptr, 0, nullptr, 0,
                                      DeviceWindow(0x1001u, 1).list(), 1).error() == -KOS_EINVAL);
        // A non-null base with size 0 is rejected at the boundary (before domain_for).
        TAP_CHECK(kos::thread::create(mmio_noop, nullptr, "mmio0", 10, KOS_POLICY_FIFO,
                                      0, false, nullptr, 0, nullptr, 0,
                                      DeviceWindow(0x2000u, 0).list(), 1).error() == -KOS_EINVAL);
        // A window whose base+size wraps the address space is rejected -KOS_EINVAL (32-bit
        // MCU; on the 64-bit sim the fail-closed encoder rejects it first, either way EINVAL).
        TAP_CHECK(kos::thread::create(mmio_noop, nullptr, "mmioW2", 10, KOS_POLICY_FIFO,
                                      0, false, nullptr, 0, nullptr, 0,
                                      DeviceWindow(0xFFFFFFF0u, 0x20).list(), 1).error() == -KOS_EINVAL);
        // The list's own refusals, ahead of any entry's geometry: a count past the bound, a
        // count with no list, the ports kind no board grants yet, an unknown kind, and a flag
        // on a device window.
        kos_window const over[1] = {};
        TAP_CHECK(kos::thread::create(mmio_noop, nullptr, "winover", 10, KOS_POLICY_FIFO, 0,
                                      false, nullptr, 0, nullptr, 0, over, UINT16_MAX).error()
                  == -KOS_ENOMEM);
        TAP_CHECK(kos::thread::create(mmio_noop, nullptr, "winnull", 10, KOS_POLICY_FIFO, 0,
                                      false, nullptr, 0, nullptr, 0, nullptr, 1).error()
                  == -KOS_EINVAL);
        kos_window const odd[] = {{0x1000u, 8u, KOS_WINDOW_PORTS, 0},
                                  {0x1000u, 0x1000u, 3u, 0},
                                  {0x1000u, 0x1000u, KOS_WINDOW_DEVICE, KOS_WINDOW_RO}};
        // Ports are x86's alone, where 0x1000 is in no aperture q35 states. The sim runs on an
        // x86 host but grants no ports.
#if defined(__x86_64__) and not KICKOS_ARCH_SIM
        int const odd_rc[] = {-KOS_EINVAL, -KOS_EINVAL, -KOS_EINVAL};
#else
        int const odd_rc[] = {-KOS_ENOTSUP, -KOS_EINVAL, -KOS_EINVAL};
        TAP_CHECK(kos_port_reg_write(0x70u, 0, 0) == -KOS_ENOSYS);
#endif
        for (int i = 0; i < 3; i++)
        {
            TAP_CHECK(kos::thread::create(mmio_noop, nullptr, "winodd", 10, KOS_POLICY_FIFO, 0,
                                          false, nullptr, 0, nullptr, 0, &odd[i], 1).error()
                      == odd_rc[i]);
        }
#if KICKOS_HAVE_ASPACE
        // A memory window must name one of the spawner's own reservations, whole.
        kos_window const mem = {0x1000u, 0x1000u, KOS_WINDOW_MEMORY, 0};
        TAP_CHECK(kos::thread::create(mmio_noop, nullptr, "winmem", 10, KOS_POLICY_FIFO, 0,
                                      false, nullptr, 0, nullptr, 0, &mem, 1).error()
                  == -KOS_EPERM);
#endif
        kos_sem_create(0, &g_mmio_done);
        g_mmio_unpriv_rc = -2;
        kos_cap_grant caps[] = {{g_mmio_done, CH_FULL}};
        auto w = kos::thread::create_caps(mmio_unpriv_worker, nullptr, "mmioW", 10, caps, 1);
        if (not w.valid())
        {
            // The three encodability cases above already ran, so this stays `ok` and names
            // the dropped half.
            tap::partial("unprivileged half not run (thread pool too small)");
            kos_sem_destroy(g_mmio_done);
            return;
        }
        kos_sem_wait(g_mmio_done);
        kos_sem_destroy(g_mmio_done);
        TAP_CHECK(g_mmio_unpriv_rc == -KOS_EPERM);
    }

    // --- stack_base arena containment (unprivileged self-grant) -----------------
    // The stack_base grant is the ONE unprivileged path that reaches an MPU region. Without
    // an arena bound an unprivileged thread spawns a child with stack_base in peripheral
    // space or kernel SRAM: an R|W window the MMIO gate would refuse. Enforcing backends
    // only: the escalation needs a region descriptor to land in.
#if KICKOS_HAVE_MPU
    int g_stkarena_rc = -2;
    kos_cap_t g_stkarena_done = KOS_CAP_NONE;
    void stkarena_noop(void*) {}
    void stkarena_unpriv_worker(void*)
    {
        // Unprivileged caller; stack_base far above any SRAM arena and naturally aligned
        // (clears the size/align/natural checks) so ONLY the arena bound can reject it.
        g_stkarena_rc = kos::thread::create(stkarena_noop, nullptr, "stkbad", 10,
                                            KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                            reinterpret_cast<void*>(0xE0000000u), 2048)
                            .error();
        kos_sem_post(CH_DONE); // g_stkarena_done
    }
    void t_stackbase_arena()
    {
        kos_sem_create(0, &g_stkarena_done);
        g_stkarena_rc = -2;
        kos_cap_grant caps[] = {{g_stkarena_done, CH_FULL}};
        auto w = kos::thread::create_caps(stkarena_unpriv_worker, nullptr, "stkW", 10, caps, 1);
        if (not w.valid())
        {
            // The unprivileged child IS this arm, so a missing thread is a whole-arm SKIP,
            // never a partial pass.
            tap::skip("thread pool too small");
            kos_sem_destroy(g_stkarena_done);
            return;
        }
        kos_sem_wait(g_stkarena_done);
        kos_sem_destroy(g_stkarena_done);
        TAP_CHECK(g_stkarena_rc == -KOS_EPERM);
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // --- Rule 7 grant predicates: the overlap matrix + RAM/DEV admission ---------
    // Exercises grant_hits_reserved / grant_region_admissible through kos_grant_probe. The
    // reserved-OVERLAP matrix needs a board that declares reserved blocks; the sim reserves
    // nothing, so that half reports PARTIAL there. The bit-band alias-hit case is HW-only.
    void grant_noop(void*) {}
    void t_grant_reserved()
    {
        // --- RAM-path admission (arena-relative; runs on sim). ---
        // kos_ram_alloc hands back a block the arch can name with one descriptor, so it is
        // admissible R|W for EVERY caller posture. The probed size must be a granule
        // multiple; the sim's granule is a 4 KiB host page.
        size_t const g = discover_granule();
        void* raw = nullptr;
        if (g != 0)
        {
            raw = kos_ram_alloc(g);
        }
        if (raw != nullptr)
        {
            uintptr_t const a = reinterpret_cast<uintptr_t>(raw);
            TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_PRIVILEGED, a, g) == 1);
            TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_UNPRIVILEGED, a, g) == 1);
            // a + 1 is sub-granule on every backend: the smallest granule in the tree
            // is PMP's 8.
            TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_PRIVILEGED, a + 1, g) == 0);  // base below the arch's region granule
        }
        else
        {
            tap::partial("arena-relative RAM cases not run (granule alloc failed)");
        }
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_PRIVILEGED, 0x1000u, 0x1000u) == 0);      // out-of-arena RAM refused
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_PRIVILEGED, 0xFFFFFFF0u, 0x20u) == 0);    // wrap (32-bit) / out-of-arena (64-bit) refused
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_PRIVILEGED, 0x20000000u, 0u) == 0);       // size 0 refused
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_DEV_UNPRIVILEGED, 0x40000000u, 0x1000u) == 0);  // DEV grant, unprivileged caller: refused

        // --- Non-cacheable admission is three-valued: PROGRAMMED and INHERENT both admit,
        // REFUSED must refuse HERE, since every commit backend drops a region it cannot
        // encode in silence.
        uintptr_t const nc = kos_grant_probe(KOS_GRANT_OP_NOCACHE_SUPPORT, 0, 0);
        TAP_CHECK(nc <= 2); // enum arch_mpu_nocache; a bad op would answer -KOS_EINVAL cast up
        if (raw != nullptr)
        {
            uintptr_t const a = reinterpret_cast<uintptr_t>(raw);
            uintptr_t expect = 1;
            if (nc == 0)
            {
                expect = 0; // ARCH_MPU_NOCACHE_REFUSED
            }
            TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_NOCACHE, a, g) == expect);
        }
        else
        {
            tap::partial("non-cacheable RAM admission not run (granule alloc failed)");
        }

        // --- End-to-end errno coherence: an unprivileged child whose mem_base lies outside
        // the arena is refused with -KOS_EPERM (policy refusal), not -KOS_ENOMEM. The code
        // must come from domain_for, not a pre-check at the spawn boundary. 0xE0000000 is
        // 2048-aligned, so ONLY arena containment can reject it.
        auto const mrc = kos::thread::create(grant_noop, nullptr, "membad", 10, KOS_POLICY_FIFO,
                                             0, /*privileged=*/false,
                                             reinterpret_cast<void*>(0xE0000000u), 2048);
        TAP_CHECK(mrc.error() == -KOS_EPERM);

        // --- Reserved-overlap matrix. ---
        // The NON-overlap cases must land in a GAP, so they anchor on scanned edges: the
        // lowest reserved base has nothing flush below it and the highest reserved end
        // nothing at-or-above it, even when blocks are flush (rp2040 TIMER abuts WATCHDOG).
        // Testing block[0]-relative "above" would false-hit on such a board.
        uintptr_t const n = kos_grant_probe(KOS_GRANT_OP_RESERVED_COUNT, 0, 0);
        if (n == 0)
        {
            tap::partial("reserved-overlap matrix not run (board reserves nothing)");
            return;
        }
        uintptr_t const rb = kos_grant_probe(KOS_GRANT_OP_RESERVED_BASE, 0, 0);       // block[0].base
        uintptr_t const rs = kos_grant_probe(KOS_GRANT_OP_RESERVED_SIZE, 0, 0);       // block[0].size
        uintptr_t const rlast = rb + rs - 1u;
        uintptr_t lo_base = rb;
        uintptr_t hi_end = rb + rs; // one-past-last
        for (uintptr_t i = 0; i < n; i++)
        {
            uintptr_t const b = kos_grant_probe(KOS_GRANT_OP_RESERVED_BASE, i, 0);
            uintptr_t const s = kos_grant_probe(KOS_GRANT_OP_RESERVED_SIZE, i, 0);
            if (b < lo_base)
            {
                lo_base = b;
            }
            if (b + s > hi_end)
            {
                hi_end = b + s;
            }
        }
        // All overlap block[0], so they hit regardless of layout.
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, rb, rs) == 1);            // equal
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, rb + 4u, 8u) == 1);       // contained
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, rb - 4u, 8u) == 1);       // partial straddle (low edge)
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, rb - 1u, 2u) == 1);       // one-byte edge low (last == rb)
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, rlast, 2u) == 1);         // one-byte edge high (base == rlast)
        // Permit (no overlap), anchored on proven gap edges:
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, lo_base - 0x10u, 0x10u) == 0); // adjacent below lowest (last == lo_base-1)
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, hi_end, 0x10u) == 0);          // adjacent above highest (base == prev last+1)
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, hi_end + 0x100000u, 0x10u) == 0); // disjoint, well clear above
        // The gap edges are genuinely adjacent: the reserved byte just inside each edge
        // still hits, so the boundary is exact and not merely an empty gap.
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, lo_base, 1u) == 1);       // first byte of the lowest block
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_HITS_RESERVED, hi_end - 1u, 1u) == 1);   // last byte of the highest block
        // Rule 7 core: a reserved block is inadmissible as a DEV grant (privileged too)
        // and as RAM.
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_DEV_PRIVILEGED, rb, rs) == 0);
        TAP_CHECK(kos_grant_probe(KOS_GRANT_OP_RAM_PRIVILEGED, rb, rs) == 0);
        // End-to-end: a privileged spawn granting the reserved MMIO window is refused.
        auto const rc = kos::thread::create(grant_noop, nullptr, "rsvd", 10, KOS_POLICY_FIFO,
                                            0, false, nullptr, 0, nullptr, 0,
                                            DeviceWindow(rb, static_cast<uint32_t>(rs)).list(), 1);
        TAP_CHECK(not rc.valid()); // reserved-block MMIO grant refused (domain_for, or non-encodable at the boundary)
    }

    // --- One holder per device window (-KOS_EBUSY) --------------------------------
    // A DEV window overlapping one a LIVE domain already holds is refused, matched on
    // RANGES: exact duplicate and partial overlap refuse, adjacent-but-disjoint admits. The
    // holder MUST stay alive for the whole matrix and parks on a semaphore, a reschedule
    // between two spawns otherwise letting it exit and turning every refusal into an
    // admission. The window is DISCOVERED; the sim admits exactly one, so a WIN-sized search
    // there reports PARTIAL while on any other enforcing board it must FAIL.
    constexpr int CH_DEVHOLD = 2; // the holder gate, delegated SECOND (done@1, hold@2)
    void devexcl_hold(void*) // caps: done@1, hold@2
    {
        kos_sem_wait(CH_DEVHOLD); // hold the window until main releases it
        kos_sem_post(CH_DONE);
    }
    void t_dev_window_exclusive()
    {
        constexpr uint32_t WIN = 0x100u; // pow2 >= 32: encodable on PMSAv7/v8 and byte-granular SYSMPU
        // Step 2*WIN so both `base` and its sibling `base + WIN` stay WIN-aligned: PMSA needs
        // natural alignment, so an unaligned base would be refused as unencodable, not held.
        kos_cap_t hold = KOS_CAP_NONE;
        if (kos_sem_create(0, &hold) != 0)
        {
            tap::fail("no semaphore for the holder gate; exclusivity cannot be staged");
            return;
        }
        kos_cap_grant hcaps[] = {{g_done, CH_FULL}, {hold, CH_FULL}};
        uintptr_t win = 0;
        kos::thread::Handle holder;
        int live = 0; // parked holders main still owes a post
        bool any_admissible = false;
        for (uintptr_t b = 0x40000000u; b < 0x40100000u; b += 2u * WIN)
        {
            if (kos_grant_probe(KOS_GRANT_OP_DEV_PRIVILEGED, b, WIN) != 1
                or kos_grant_probe(KOS_GRANT_OP_DEV_PRIVILEGED, b + WIN, WIN) != 1)
            {
                continue; // reserved / alias / non-encodable on this chip
            }
            any_admissible = true;
            holder = kos::thread::create(devexcl_hold, nullptr, "devheld", 10, KOS_POLICY_FIFO,
                                         0, /*privileged=*/false, nullptr, 0, nullptr, 0,
                                         DeviceWindow(b, WIN).list(), 1, hcaps, 2);
            if (holder.valid())
            {
                live++;
                win = b;
                break;
            }
            if (holder.error() != -KOS_EBUSY)
            {
                break; // not "a board service owns this window": a real refusal, report it
            }
        }
        if (not any_admissible)
        {
            // Positively the sim, so a new arch with no DEV encoder fails loudly here.
#if KICKOS_ARCH_SIM
            // The sim admits exactly one DEV window shape (64 KiB), never a WIN-sized one.
            // Assert that premise instead of skipping: the sim gate reads a skip as an arm
            // that stopped running (FAIL_REGULAR_EXPRESSION "# skipped: [1-9]").
            TAP_CHECK(not kos::thread::create(devexcl_hold, nullptr, "devnone", 10,
                                              KOS_POLICY_FIFO, 0, false, nullptr, 0, nullptr, 0,
                                              DeviceWindow(0x40000000u, WIN).list(), 1,
                                              hcaps, 2)
                              .valid());
            tap::partial("board mints no DEV window; exclusivity runs on enforcing boards "
                         "(e.g. the qemu base variant)");
#else
            // An enforcing board with no admissible DEV base means kos_grant_probe or the
            // discovery loop regressed. Passing here would silently drop every -KOS_EBUSY
            // assertion below while the gate stayed green.
            tap::fail("no DEV-admissible window in [0x40000000, 0x40100000): "
                      "kos_grant_probe or window discovery regressed");
#endif
            kos_sem_destroy(hold);
            return;
        }
        if (win == 0)
        {
            kos_sem_destroy(hold);
            if (holder.error() != -KOS_EBUSY)
            {
                // EPERM/EINVAL here is a boundary failure and not an unrunnable case, so
                // it fails the arm instead of skipping it.
                tap::fail("DEV holder spawn refused rc %d, expected 0 or -KOS_EBUSY",
                          holder.error());
                return;
            }
            tap::skip("every DEV-admissible window is already held (holder rc %d)",
                      holder.error());
            return;
        }
        // 0. One list naming the free neighbour twice: nothing else holds it, so only the
        //    check within the list can refuse it.
        kos_window const twice[] = {DeviceWindow(win + WIN, WIN).w,
                                    DeviceWindow(win + WIN, WIN).w};
        TAP_CHECK(kos::thread::create(devexcl_hold, nullptr, "devtwice", 10, KOS_POLICY_FIFO,
                                      0, false, nullptr, 0, nullptr, 0, twice, 2, hcaps, 2)
                      .error()
                  == -KOS_EBUSY);
        // 1. Exact duplicate of the live holder's window: refused, and specifically
        //    EBUSY, not EPERM (the window is admissible) and not ENOMEM (the pool has
        //    room; the refusal lands before a slot is claimed).
        TAP_CHECK(kos::thread::create(devexcl_hold, nullptr, "devdup", 10, KOS_POLICY_FIFO,
                                      0, false, nullptr, 0, nullptr, 0,
                                      DeviceWindow(win, WIN).list(), 1, hcaps, 2).error() == -KOS_EBUSY);
        // 2. Partial overlap: the upper half of the held window. Its own base/size are
        //    independently admissible, so only the overlap scan can refuse it.
        TAP_CHECK(kos::thread::create(devexcl_hold, nullptr, "devpart", 10, KOS_POLICY_FIFO,
                                      0, false, nullptr, 0, nullptr, 0,
                                      DeviceWindow(win + WIN / 2u, WIN / 2u).list(), 1, hcaps, 2).error()
                  == -KOS_EBUSY);
        // 3. Adjacent but disjoint (base == held last + 1): admitted. This is the mk64f PIT
        //    CH2 shape: a grant flush against a block, which must not be read as overlapping.
        auto const adj = kos::thread::create(devexcl_hold, nullptr, "devadj", 10, KOS_POLICY_FIFO,
                                             0, false, nullptr, 0, nullptr, 0,
                                             DeviceWindow(win + WIN, WIN).list(), 1, hcaps, 2);
        TAP_CHECK(adj.valid());
        if (adj.valid())
        {
            live++;
        }
        // The post is what frees the windows, so the drain does not depend on timing.
        for (int i = 0; i < live; i++)
        {
            kos_sem_post(hold);
        }
        wait_n(live);
        // The SAME grant refused above must now succeed, so the refusal tracked live holders
        // and not the address, and it succeeds as one list with its neighbour, whose second
        // entry is then held as the first is. CH_DONE is posted before the holder returns, so
        // retry rather than assume the domain is already released.
        kos_window const both[] = {DeviceWindow(win, WIN).w, DeviceWindow(win + WIN, WIN).w};
        int again = -KOS_EBUSY;
        for (int i = 0; i < 100 and again == -KOS_EBUSY; i++)
        {
            again = kos::thread::create(devexcl_hold, nullptr, "devagain", 10, KOS_POLICY_FIFO,
                                        0, false, nullptr, 0, nullptr, 0, both, 2, hcaps, 2)
                        .error();
            if (again == -KOS_EBUSY)
            {
                kos_sleep_ns(1000000ull); // 1 ms
            }
        }
        TAP_CHECK(again == 0);
        if (again == 0)
        {
            TAP_CHECK(kos::thread::create(devexcl_hold, nullptr, "devsecond", 10,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, nullptr, 0,
                                          DeviceWindow(win + WIN, WIN).list(), 1, hcaps, 2)
                          .error()
                      == -KOS_EBUSY);
            kos_sem_post(hold); // drain it too, so later tests get the slot back
            wait_n(1);
        }
        kos_sem_destroy(hold);
    }
#endif // KICKOS_ENABLE_SELFTEST
#endif // KICKOS_HAVE_MPU

    // --- Confused-deputy readable-buffer floor ---------------------------------
    // A user pointer syscall_dispatch READS must lie in memory the unprivileged caller could
    // itself reach. A rodata string literal MUST be accepted and a pointer into no granted
    // region MUST be rejected, never read; both run from a spawned unprivileged worker. The
    // positive half is non-vacuous only when PAIRED with the guard-page negative below.
    char const CD_LIT[] = "# [confdep] unpriv rodata buffer accepted by the readable floor\n";
    // worker: kconsole_write(rodata literal) -> a count, -KOS_EAGAIN or -KOS_EBUSY
    long g_cd_lit_rc = -99;
    int g_cd_goodspawn = -99;  // worker: spawn rc of a child NAMED from .rodata
    int g_cd_goodname_ran = 0; // that child ran (name-copy path did not break spawn)
    kos_cap_t g_cd_kidsem = KOS_CAP_NONE; // grandchild -> worker handoff
    kos_cap_t g_cd_done = KOS_CAP_NONE;   // worker -> main
#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
    int g_cd_neg_ran = 0;       // the negative half actually ran (guard page available)
    long g_cd_bad_rc = -99;     // worker: kconsole_write(guard page) -> expect 0 (rejected)
    int g_cd_badname_spawn = -99; // spawn rc with a BOGUS name pointer -> expect 0
    int g_cd_badname_ran = 0;   // that child ran (kernel walked the bad name safely)
#endif
    void cd_kid(void* arg) // caps: g_cd_kidsem@1 (CH_DONE), delegated by cd_worker
    {
        *static_cast<int*>(arg) = 1;
        kos_sem_post(CH_DONE); // g_cd_kidsem (this grandchild's delegated cap)
    }
    void cd_worker(void*) // UNPRIVILEGED; caps: g_cd_done@1 (CH_DONE), delegated by main
    {
        size_t const lit_len = strlen(CD_LIT);
        int32_t const lit_took = kos_kconsole_write(CD_LIT, lit_len);
        g_cd_lit_rc = lit_took;
        write_rest(CD_LIT, lit_len, lit_took);

        // cd_worker creates its OWN sem (unprivileged create is allowed) and RE-delegates
        // it to a grandchild: nested delegation requires the source cap carry TRANSFER,
        // which sem_create grants. g_cd_kidsem is cd_worker's cap value (its table).
        kos_sem_create(0, &g_cd_kidsem);
        kos_cap_grant kidcaps[] = {{g_cd_kidsem, CH_FULL}};  // grandchild's index 1
        // A child NAMED from .rodata: the kernel bounds + copies the string. Userspace
        // cannot read a TCB name back, so acceptance shows as the child running.
        g_cd_goodspawn = kos::thread::create_caps(cd_kid, &g_cd_goodname_ran, "cdgood", 9,
                                                  kidcaps, 1)
                             .error();
        if (g_cd_goodspawn == 0)
        {
            kos_sem_wait(g_cd_kidsem);
        }
#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
        void* bad = kos_guard_addr(); // an arena page granted to no domain
        if (bad != nullptr)
        {
            // Bogus console buffer: rejected, and never read (a wrong-accept would return 8,
            // having read the guard page the caller cannot reach).
            g_cd_bad_rc = kos_kconsole_write(bad, 8);
            // Bogus NAME pointer: the kernel must bound the walk (no fault), drop the
            // name, and still spawn the child.
            g_cd_badname_spawn = kos::thread::create_caps(cd_kid, &g_cd_badname_ran,
                                                          static_cast<char const*>(bad), 9,
                                                          kidcaps, 1)
                                     .error();
            if (g_cd_badname_spawn == 0)
            {
                kos_sem_wait(g_cd_kidsem);
            }
            g_cd_neg_ran = 1;
        }
#endif
        kos_sem_destroy(g_cd_kidsem); // close cd_worker's own cap
        kos_sem_post(CH_DONE);        // g_cd_done (delegated from main)
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // Read doorbell service and instruction-rendezvous counters. Assert a service floor
    // on every released core and the pairing; report rendezvous growth without requiring
    // it, since executable-space teardown depends on scheduling.
    // Distinguish this kernel's cores from rows belonging to other AMP nodes.
    bool doorbell_core_is_mine(unsigned c, unsigned me)
    {
#if KICKOS_AMP_OWN_IMAGE
        (void)0;
        return c == me;
#else
        (void)me;
        return c < static_cast<unsigned>(KICKOS_KERNEL_CORES);
#endif
    }

    // --- A line the arch dispatches to a kernel vector of its own --------------
    // Its vector reaches a kernel service directly and never kickos_isr_irq, so it holds no
    // irq_table slot: a capability over it would drive nothing, and closing that capability
    // would detach and MASK the line the kernel rings its peers on. The kernel refuses the
    // claim; the ordinary line below is what says the refusal is that line's and not every
    // line's.
    void t_irq_kernel_line_reserved()
    {
        int64_t const owned =
            static_cast<int64_t>(kos_doorbell_probe(KOS_DOORBELL_OP_KERNEL_LINE, 0));
        tap::diag("arch-owned irq line: %ld", static_cast<long>(owned));
        if (owned >= 0)
        {
            kos_cap_t reserved = KOS_CAP_NONE;
            TAP_CHECK(kos_irq_claim(static_cast<int>(owned), KOS_IRQ_EDGE, &reserved)
                      == -KOS_EPERM);
            TAP_CHECK(reserved == KOS_CAP_NONE);
            // The inject refuses the same line too: raising the tick or the doorbell from
            // userspace would reach kernel state no capability named.
            TAP_CHECK(kos_irq_inject(static_cast<int>(owned)) == -KOS_EPERM);
            // And no second line is reserved behind it, which a sweep from one past it says.
            TAP_CHECK(static_cast<int64_t>(kos_doorbell_probe(KOS_DOORBELL_OP_KERNEL_LINE,
                                                              static_cast<uintptr_t>(owned) + 1u))
                      < 0);
        }
        else
        {
            // A partial and never a plain pass: the control below still runs, so this is not a
            // skip, but a controller reserving no line leaves the refusal above no subject and
            // an unqualified `ok` would put that absence outside both bookkeeping sets.
            tap::partial("this arch routes every line through the first-level ISR");
        }
        // CLAIM_GATE_LINE, free outside its own arm: claimed, attached, armed, fired and
        // closed here.
        kos_cap_t line = KOS_CAP_NONE;
        TAP_CHECK(kos_irq_claim(CLAIM_GATE_LINE, KOS_IRQ_EDGE, &line) == 0);
        kos_cap_t note = notify_for_line(line);
        TAP_CHECK(note != KOS_CAP_NONE);
        TAP_CHECK(kos_notify_bind(note) == 0);
        TAP_CHECK(kos_irq_ack(line) == 0); // a claim leaves the line masked
        // The control for the refusal above: an ordinary line still injects, from a caller
        // holding no authority at all, which is what the inject is for.
        TAP_CHECK(kos_irq_inject(CLAIM_GATE_LINE) == 0);
        uint32_t bits = 0;
        TAP_CHECK(kos_notify_wait(note, NOTE_ALL, 0u, &bits) == 0 and bits != 0);
        TAP_CHECK(kos_irq_ack(line) == 0);
        TAP_CHECK(kos_notify_unbind(note) == 0);
        TAP_CHECK(kos_handle_close(note) == 0);
        TAP_CHECK(kos_handle_close(line) == 0);
    }

    void t_doorbell_xpoke()
    {
        // The matrix's own width and this core's own row, both asked of the kernel: the width
        // is the MACHINE's core count and not the cores this image drives, and row 0 is a
        // peer's row on every node but the first.
        unsigned const width =
            static_cast<unsigned>(kos_doorbell_probe(KOS_DOORBELL_OP_WIDTH, 0));
        unsigned const me =
            static_cast<unsigned>(kos_doorbell_probe(KOS_DOORBELL_OP_SELF, 0));
        TAP_CHECK(width >= static_cast<unsigned>(KICKOS_NUM_CORES));
        TAP_CHECK(me < width);
        // On the own-image posture the partition's map says which row is this node's. The
        // bound above is satisfied by row 0 on every node, so it alone asserts nothing here.
#if KICKOS_AMP_OWN_IMAGE
        TAP_CHECK(me == static_cast<unsigned>(KICKOS_AMP_SELF_CORE));
#endif
        // And it is a core THIS kernel schedules on, which under a shared kernel is a claim
        // about the matrix's low rows.
        TAP_CHECK(doorbell_core_is_mine(me, me));

        uint32_t served = 0;
        uint32_t initiated = 0;
        for (unsigned c = 0; c < width; c++)
        {
            uint64_t const w = kos_doorbell_probe(KOS_DOORBELL_OP_COUNTS, c);
            served += static_cast<uint32_t>(w & 0xFFFFFFFFu);
            initiated += static_cast<uint32_t>(w >> 32);
        }

        for (unsigned i = 0; i < 4u; i++)
        {
            kos_task_t t = KOS_TASK_NONE;
            if (kos_task_create(nullptr, 0, 0, &t) != 0)
            {
                break;
            }
            kos_yield();
            (void)kos_task_kill(t);
            kos_yield();
        }

        uint32_t served_after = 0;
        uint32_t initiated_after = 0;
        uint32_t peer_served = 0;
        uint32_t mine_served = 0;
        [[maybe_unused]] unsigned silent_rows = 0;
        for (unsigned c = 0; c < width; c++)
        {
            uint64_t const w = kos_doorbell_probe(KOS_DOORBELL_OP_COUNTS, c);
            uint32_t const s = static_cast<uint32_t>(w & 0xFFFFFFFFu);
            uint32_t const n = static_cast<uint32_t>(w >> 32);
            char const* whose = "peer";
            if (c == me)
            {
                whose = "self";
            }
            else if (doorbell_core_is_mine(c, me))
            {
                whose = "mine";
            }
            tap::diag("core %u (%s): %u service(s), %u rendezvous initiated", c, whose,
                      static_cast<unsigned>(s), static_cast<unsigned>(n));
            if (doorbell_core_is_mine(c, me))
            {
                if (s == 0u)
                {
                    silent_rows |= 1u << c;
                }
                mine_served += s;
            }
            else
            {
                peer_served += s;
            }
            served_after += s;
            initiated_after += n;
        }
        tap::diag("doorbell matrix %u row(s), this core row %u: services %u -> %u, rendezvous "
                  "%u -> %u, mine %u, peers %u",
                  width, me, static_cast<unsigned>(served), static_cast<unsigned>(served_after),
                  static_cast<unsigned>(initiated), static_cast<unsigned>(initiated_after),
                  static_cast<unsigned>(mine_served), static_cast<unsigned>(peer_served));

        // Monotonic: a counter that went backwards is a torn read of a cell with one writer.
        TAP_CHECK(served_after >= served);
        TAP_CHECK(initiated_after >= initiated);
#if KICKOS_KERNEL_CORES > 1
        // Only a core the bring-up check released owes a service: row 0 runs that check, and a
        // reschedule raise carries no rendezvous, so nothing is certain to ever ask row 0 one.
        TAP_CHECK((silent_rows & ~1u) == 0u);
        // Each rendezvous pokes at least one peer and waits for it, so the services performed
        // across the machine cannot be fewer than the rendezvous initiated.
        TAP_CHECK(served_after >= initiated_after);
#elif KICKOS_AMP_NODE
        // An AMP node at one kernel core. arch.h guards the doorbell on
        // (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE), so an own-image node drives one core and
        // still has a live doorbell over a matrix its partition spans; keyed on the count
        // instead, this branch would assert the FOLD on the one posture built to be rung.
        //
        // An instruction-side poke is owed only where a PEER holds one of ITS spaces, and a
        // node whose peers run kernels of their own has no such peer.
        TAP_CHECK(initiated_after == 0u);
        TAP_CHECK(served_after >= initiated_after);
#if !KICKOS_AMP_OWN_IMAGE
        // Under one image the peers are this image's own cores, parked with the doorbell open
        // and poked by the bring-up check before the kernel starts, so a zero here says the
        // doorbell never reached one. The services land on the peer rows and none on the
        // kernel's own, which is why the claim is the peers'.
        TAP_CHECK(peer_served > 0u);
#else
        // Under one image per node there is no positive service claim to make here: this arm
        // runs ahead of every arm that rings anything, so the matrix reads zero on a node
        // booted alone and in a merged artefact whose peer only answers. What the arm carries
        // on this posture is the KEYING: the matrix's own width, this core's own row, and
        // which rows are peers.
#endif
#else
        // One core and no partition: the mechanism folds out of the image, so the probe reads
        // exactly zero rather than an idle counter, and the one core the kernel schedules on is
        // counted silent for the same reason.
        TAP_CHECK(served_after == 0u);
        TAP_CHECK(initiated_after == 0u);
        TAP_CHECK(silent_rows == 1u);
#endif
    }
#endif


#if defined(KICKOS_ENABLE_SELFTEST)
    // --- Placement and the scheduling grant ----------------------------------------------
    constexpr uint8_t PC_CEILING = 5;


    void pc_idle_worker(void*)
    {
    }

    void t_prio_ceiling_refused()
    {
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            tap::skip("task pool too small");
            return;
        }
        int const granted = kos_task_sched_grant(t, PC_CEILING, 0);
        auto above = kos::thread::create_caps(pc_idle_worker, nullptr, "pcabv",
                                              PC_CEILING + 1, nullptr, 0, KOS_POLICY_FIFO, 0,
                                              false, nullptr, 0, 0, nullptr, t);
        bool const above_valid = above.valid();
        int const above_err = above.error();
        auto at = kos::thread::create_caps(pc_idle_worker, nullptr, "pcat", PC_CEILING,
                                           nullptr, 0, KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                           0, nullptr, t);
        bool const at_valid = at.valid();
        int at_join = -1;
        if (at_valid)
        {
            at_join = at.join();
        }
        (void)kos_task_kill(t);
        TAP_CHECK(granted == 0);
        TAP_CHECK(not above_valid);
        TAP_CHECK(above_err == -KOS_EPERM);
        TAP_CHECK(at_valid);
        TAP_CHECK(at_join == 0);
    }

    enum
    {
        PN_MADE = 0,  // the member got a task of its own to narrow
        PN_ABOVE = 1, // a ceiling above the member's own
        PN_SAME = 2,  // ... and one equal to it
        PN_WORDS = 3
    };
    // Refusals only, no probe: KOS_SYS_SCHED_PROBE is compiled out at one kernel core and this
    // arm runs on every posture.
    void pc_narrow_worker(void*) // caps: E(SIGNAL)@1
    {
        int32_t rep[PN_WORDS] = {0, -99, -99};
        kos_task_t u = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &u) == 0)
        {
            rep[PN_MADE] = 1;
            rep[PN_ABOVE] = kos_task_sched_grant(u, PC_CEILING + 1, 0);
            rep[PN_SAME] = kos_task_sched_grant(u, PC_CEILING, 0);
            (void)kos_task_kill(u);
        }
        (void)kos_send(1, rep, sizeof(rep));
    }

    void t_prio_ceiling_narrow_only()
    {
        if (kos_endpoint_create(&g_pl_ep) != 0)
        {
            tap::skip("endpoint pool too small");
            return;
        }
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            (void)kos_handle_close(g_pl_ep);
            tap::skip("task pool too small");
            return;
        }
        int const granted = kos_task_sched_grant(t, PC_CEILING, 0);
        // The same task, still empty: narrowing-only is checked against the TASK's ceiling
        // too, so main widening one it already narrowed is refused although main's own
        // ceiling admits it.
        int const widen = kos_task_sched_grant(t, PC_CEILING + 1, 0);
        kos_cap_grant caps[] = {{g_pl_ep, KOS_CAP_SIGNAL}};
        // The member creates a task of its own to narrow, so it holds the task authority.
        auto m = kos::thread::create_caps(pc_narrow_worker, nullptr, "pcnar", PC_CEILING,
                                          caps, 1, KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                          KOS_AUTH_TASKS, nullptr, t);
        bool const seated = m.valid();
        int32_t rep[PN_WORDS] = {0, 0, 0};
        bool heard = false;
        int join_rc = -1;
        if (seated)
        {
            struct kos_reply_recv_opts o;
            kos_reply_recv_opts_init(&o, g_pl_ep, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
            heard = kos_reply_recv(KOS_CAP_NONE, rep, kos_call_lens_pack(0, sizeof(rep)), &o)
                    == static_cast<int32_t>(sizeof(rep));
            join_rc = m.join();
        }
        (void)kos_task_kill(t);
        (void)kos_handle_close(g_pl_ep);
        TAP_CHECK(granted == 0);
        TAP_CHECK(widen == -KOS_EPERM);
        TAP_CHECK(seated);
        TAP_CHECK(heard);
        TAP_CHECK(join_rc == 0);
        TAP_CHECK(rep[PN_MADE] == 1);
        TAP_CHECK(rep[PN_ABOVE] == -KOS_EPERM);
        TAP_CHECK(rep[PN_SAME] == 0);
    }
#endif


    // --- One buffer named by both ends of a rendezvous ---------------------------------
    // ep_copy refuses a copy whose two ends are the same memory under one owner, because the
    // primitive under it is the ascending-only kmemcpy. Two threads of ONE task reach that:
    // they share a space, so one static array is one address for both, and neither end had to
    // be granted anything. The refusal must reach BOTH of them as -KOS_EFAULT.
    //
    // Order-free: whichever of the two parks first, the other's copy is the one refused, and
    // both answer alike.
    constexpr size_t SB_LEN = 32;
    char g_sb_buf[SB_LEN];
    int32_t g_sb_sent = 1; // 1 is no answer a send can give
    void sb_sender(void*)  // caps: done@1, E(SIGNAL)@2
    {
        g_sb_sent = kos_send(2, g_sb_buf, SB_LEN);
        kos_sem_post(CH_DONE);
    }
    void t_ipc_one_buffer_both_ends()
    {
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            tap::skip("endpoint pool too small");
            return;
        }
        for (size_t i = 0; i < SB_LEN; i++)
        {
            g_sb_buf[i] = static_cast<char>('a' + (i & 15u));
        }
        g_sb_sent = 1;
        // No task named, so the child is a thread of main's task and the array below is one
        // address in one space.
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {ep, KOS_CAP_SIGNAL}};
        if (not kos::thread::create_caps(sb_sender, nullptr, "sbuf", 10, caps, 2).valid())
        {
            (void)kos_handle_close(ep);
            tap::skip("thread pool too small");
            return;
        }
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, ep, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        int32_t const got =
            kos_reply_recv(KOS_CAP_NONE, g_sb_buf, kos_call_lens_pack(0, SB_LEN), &o);
        wait_n(1);
        (void)kos_handle_close(ep);
        tap::diag("one buffer both ends: recv %d, send %d",
                  static_cast<int>(got), static_cast<int>(g_sb_sent));
        TAP_CHECK(got == -KOS_EFAULT);
        TAP_CHECK(g_sb_sent == -KOS_EFAULT);
    }

    void t_confused_deputy()
    {
        kos_sem_create(0, &g_cd_done);
        kos_cap_grant caps[] = {{g_cd_done, CH_FULL}};
        auto w = kos::thread::create_caps(cd_worker, nullptr, "cdwork", 10, caps, 1);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            kos_sem_destroy(g_cd_done);
            return;
        }
        kos_sem_wait(g_cd_done);
        kos_sem_destroy(g_cd_done);
        // Positive (every backend): the floor accepted an unprivileged caller's rodata
        // pointer. kos_kconsole_write answers a short count or -KOS_EAGAIN when the console
        // cannot take the bytes, -KOS_EBUSY when a published driver serves the caller, and
        // -KOS_EFAULT when it rejects the buffer, so only a rejection reports on the readable
        // floor this arm is named for.
        if (g_cd_lit_rc < 0 and g_cd_lit_rc != -KOS_EAGAIN and g_cd_lit_rc != -KOS_EBUSY)
        {
            tap::fail("readable floor refused an unprivileged rodata buffer: rc %ld",
                      g_cd_lit_rc);
            return;
        }
        // The grandchild needs its own stack, and on a 16 KiB-SRAM part that alloc can fail.
        // The rodata-literal positive above already covered the read path.
        if (g_cd_goodspawn != 0)
        {
            tap::partial("grandchild-name half not run (arena too small for its stack)");
            return;
        }
        TAP_CHECK(g_cd_goodname_ran == 1);
#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
        // Negative (enforcing backend): a bogus buffer/name is rejected, never read,
        // and never faults the kernel.
        if (g_cd_neg_ran)
        {
            TAP_CHECK(g_cd_bad_rc == -KOS_EFAULT); // bogus buffer rejected, never read
            TAP_CHECK(g_cd_badname_spawn == 0 and g_cd_badname_ran == 1);
        }
#endif
    }

    // --- Endpoint IPC: synchronous rendezvous send/recv ----------
    // The endpoint cap is delegated to workers at child index 2 (done@1, E@2). Workers
    // are UNPRIVILEGED so the kernel's copy into/from a parked peer runs against real
    // enforcement (the cross-domain privileged write).
    char const EP_MSG[] = "hello-endpoint"; // no NUL sent
    constexpr uint8_t EP_SIGNAL_ONLY = KOS_CAP_SIGNAL;
    constexpr uint8_t EP_WAIT_ONLY = KOS_CAP_WAIT;
    kos_cap_t g_ep = KOS_CAP_NONE; // main's endpoint cap (created per test)
    char g_ep_rbuf[64];
    Atomic<int32_t, Order::RELAXED> g_ep_rn{-99};         // worker recv return
    Atomic<uint32_t, Order::RELAXED> g_ep_rbadge{0xffffffffu};
    Atomic<int, Order::RELAXED> g_ep_rcap{64}; // capacity the recv worker passes
    Atomic<int32_t, Order::RELAXED> g_ep_sn{-99}; // worker send return

    void ep_recv_worker(void*) // caps: done@1, E@2 (unpriv)
    {
        // Keep the recv buffer thread-private: a global one is also accepted
        // (user_writable_ok has a static-data fallback, covered by writable_global) and
        // would make this arm about the writable check instead of the rendezvous.
        char buf[64];
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, 2, 0, KOS_TIMEOUT_NONE);
        o.info.badge = 0xdeadu; // sentinel: both outputs must be overwritten
        o.info.reply_cap = 0x55;
        int32_t n = kos_reply_recv(KOS_CAP_NONE, buf,
                                   kos_call_lens_pack(0, static_cast<size_t>(g_ep_rcap)), &o);
        g_ep_rn = n;
        g_ep_rbadge = o.info.badge;
        size_t k = 0;
        if (n > 0)
        {
            k = static_cast<size_t>(n);
            if (k > sizeof(buf))
            {
                k = sizeof(buf);
            }
            memcpy(g_ep_rbuf, buf, k);
        }
        kos_sem_post(CH_DONE);
    }
    void ep_send_worker(void*) // caps: done@1, E@2 (unpriv)
    {
        g_ep_sn = kos_send(2, EP_MSG, strlen(EP_MSG));
        kos_sem_post(CH_DONE);
    }

    void t_endpoint_rendezvous()
    {
        size_t const mlen = strlen(EP_MSG);
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, CH_FULL}};

        // (A) receiver parks first; sender (main) delivers into the parked buffer.
        g_ep_rn = -99; g_ep_rbadge = 0xdeadu; g_ep_rcap = 64;
        auto w = kos::thread::create_caps(ep_recv_worker, nullptr, "eprx", 12, caps, 2,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        kos_sleep_ns(3000000ull); // let the worker park in recv
        int32_t sc = kos_send(g_ep, EP_MSG, mlen);
        TAP_CHECK(sc == static_cast<int32_t>(mlen));
        wait_n(1);
        int32_t const ep_rn_a = g_ep_rn;
        TAP_CHECK(ep_rn_a == static_cast<int32_t>(mlen) and memcmp(g_ep_rbuf, EP_MSG, mlen) == 0);
        uint32_t const ep_rbadge = g_ep_rbadge;
        TAP_CHECK(ep_rbadge == 0); // badge always written on success; unbadged is 0

        // (B) sender parks first; receiver (main) takes from the parked buffer.
        g_ep_sn = -99;
        auto w2 = kos::thread::create_caps(ep_send_worker, nullptr, "eptx", 12, caps, 2,
                                           KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w2.valid());
        kos_sleep_ns(3000000ull); // let the worker park in send
        char rbuf[64];
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, g_ep, 0, KOS_TIMEOUT_NONE);
        o.info.badge = 0xdeadu; // sentinel for write-back
        o.info.reply_cap = 0x55;
        int32_t rc = kos_reply_recv(KOS_CAP_NONE, rbuf, kos_call_lens_pack(0, sizeof(rbuf)), &o);
        TAP_CHECK(rc == static_cast<int32_t>(mlen) and memcmp(rbuf, EP_MSG, mlen) == 0);
        TAP_CHECK(o.info.badge == 0);
        wait_n(1);
        int32_t const ep_sn = g_ep_sn;
        TAP_CHECK(ep_sn == static_cast<int32_t>(mlen));

        // (C) zero-length is a valid signal, not an error.
        g_ep_rn = -99; g_ep_rcap = 64;
        auto w3 = kos::thread::create_caps(ep_recv_worker, nullptr, "epz", 12, caps, 2,
                                           KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w3.valid());
        kos_sleep_ns(3000000ull);
        TAP_CHECK(kos_send(g_ep, EP_MSG, 0) == 0);
        wait_n(1);
        int32_t const ep_rn_c = g_ep_rn;
        TAP_CHECK(ep_rn_c == 0);

        // (D) truncation: a capacity below the message length.
        g_ep_rn = -99; g_ep_rcap = 4;
        auto w4 = kos::thread::create_caps(ep_recv_worker, nullptr, "eptr", 12, caps, 2,
                                           KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w4.valid());
        kos_sleep_ns(3000000ull);
        TAP_CHECK(kos_send(g_ep, EP_MSG, mlen) == 4);
        wait_n(1);
        int32_t const ep_rn_d = g_ep_rn;
        TAP_CHECK(ep_rn_d == 4 and memcmp(g_ep_rbuf, EP_MSG, 4) == 0);

        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    // --- Oversize reject + bad cap (main only; no parking) -----------------------
    void t_endpoint_reject()
    {
        char big[KOS_EP_MSG_MAX + 8];
        memset(big, 'x', sizeof(big));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        // Oversize send is rejected up front with -KOS_EINVAL, WITHOUT parking (main is
        // the sole WAIT holder, so a park would hang the suite).
        TAP_CHECK(kos_send(g_ep, big, KOS_EP_MSG_MAX + 1) == -KOS_EINVAL);
        // Bad caps reject at the resolve boundary on both paths with -KOS_EBADF.
        char one[1] = {0};
        TAP_CHECK(kos_send(0x7fffffff, one, 1) == -KOS_EBADF);
        struct kos_reply_recv_opts bad_ep;
        kos_reply_recv_opts_init(&bad_ep, 0x7fffffff, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, g_ep_rbuf, kos_call_lens_pack(0, 1), &bad_ep)
                  == -KOS_EBADF);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    // --- Rights denial: send needs SIGNAL, recv needs WAIT -----------------------
    Atomic<int, Order::RELAXED> g_ep_wait_send_rc{-99};   // WAIT-only cap send -> -KOS_EACCES
    Atomic<int, Order::RELAXED> g_ep_signal_recv_rc{-99}; // SIGNAL-only cap recv -> -KOS_EACCES
    void ep_rights_worker(void*) // caps: done@1, E(WAIT)@2, E(SIGNAL)@3
    {
        char b[8] = {0};
        g_ep_wait_send_rc = static_cast<int>(kos_send(2, b, 1));
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, 3, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        g_ep_signal_recv_rc = static_cast<int>(
            kos_reply_recv(KOS_CAP_NONE, b, kos_call_lens_pack(0, sizeof(b)), &o));
        kos_sem_post(CH_DONE);
    }
    void t_endpoint_rights()
    {
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        g_ep_wait_send_rc = -99; g_ep_signal_recv_rc = -99;
        // Two narrowed caps to the same endpoint: WAIT-only at index 2, SIGNAL-only at 3.
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}, {g_ep, EP_SIGNAL_ONLY}};
        auto w = kos::thread::create_caps(ep_rights_worker, nullptr, "eprt", 12, caps, 3,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        wait_n(1);
        int const ep_wait_send_rc = g_ep_wait_send_rc;
        int const ep_signal_recv_rc = g_ep_signal_recv_rc;
        TAP_CHECK(ep_wait_send_rc == -KOS_EACCES);
        TAP_CHECK(ep_signal_recv_rc == -KOS_EACCES);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    // --- A parked sender is refused when the last WAIT holder drops it -------------------
    // A SIGNAL-only delegation does NOT bump recv_holders, so main's cap is the sole WAIT
    // holder: closing it takes recv_holders 1->0, and with the handout right gone with it the
    // parked sender is woken -KOS_ECONNREFUSED.
    Atomic<int32_t, Order::RELAXED> g_ep_epipe_rc{-99};
    void ep_epipe_worker(void*) // caps: done@1, E(SIGNAL)@2
    {
        g_ep_epipe_rc = kos_send(2, EP_MSG, strlen(EP_MSG)); // parks
        kos_sem_post(CH_DONE);
    }
    void t_endpoint_epipe()
    {
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        g_ep_epipe_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto w = kos::thread::create_caps(ep_epipe_worker, nullptr, "epep", 12, caps, 2,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        kos_sleep_ns(3000000ull);              // let the sender park (recv_holders == 1 == main)
        TAP_CHECK(kos_handle_close(g_ep) == 0); // last WAIT cap: the parked sender is refused
        wait_n(1);
        int32_t const ep_epipe_rc = g_ep_epipe_rc;
        TAP_CHECK(ep_epipe_rc == -KOS_ECONNREFUSED);
    }

    // --- Dead endpoint (unparked): send after the last WAIT cap is gone -> refused -----
    // Distinct from the parked case: no receiver is checked before the park, not after.
    Atomic<int32_t, Order::RELAXED> g_ep_dead_rc{-99};
    kos_cap_t g_ep_go = KOS_CAP_NONE;
    void ep_dead_worker(void*) // caps: done@1, E(SIGNAL)@2, go@3
    {
        kos_sem_wait(3);                                     // go: main has dropped its WAIT cap
        g_ep_dead_rc = kos_send(2, EP_MSG, strlen(EP_MSG)); // recv_holders == 0: refused now
        kos_sem_post(CH_DONE);
    }
    void t_endpoint_dead()
    {
        int const eprc = kos_endpoint_create(&g_ep);
        int const gorc = kos_sem_create(0, &g_ep_go);
        TAP_CHECK(eprc == 0 and gorc == 0);
        g_ep_dead_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}, {g_ep_go, CH_FULL}};
        auto w = kos::thread::create_caps(ep_dead_worker, nullptr, "epde", 12, caps, 3,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        // Close main's (only) WAIT cap FIRST: recv_holders -> 0, no sender parked yet.
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        kos_sem_post(g_ep_go); // now the worker sends into the dead endpoint
        wait_n(1);
        int32_t const ep_dead_rc = g_ep_dead_rc;
        TAP_CHECK(ep_dead_rc == -KOS_ECONNREFUSED); // no receiver: rejected at once, never parked
        kos_sem_destroy(g_ep_go);
    }

    // --- The handout right: an endpoint with no receiver answers by whether one may come --
    // Main narrows its creator cap to SIGNAL, TRANSFER and HANDOUT. No receiver remains, so a
    // send answers -KOS_EAGAIN at once instead of parking, and main cannot receive through it.
    // It still seats a receiver, granting WAIT it does not hold. Until that worker first
    // waits on the endpoint a send answers -KOS_EAGAIN as well; once it waits, the worker
    // takes the next send. With the worker gone a send answers -KOS_EAGAIN again; with the
    // handout right dropped, -KOS_ECONNREFUSED, and a WAIT grant from the cap is refused.
    constexpr uint8_t EP_HANDOUT_ONLY = KOS_CAP_SIGNAL | KOS_CAP_TRANSFER | KOS_CAP_HANDOUT;
    // Sends a millisecond apart, until the seated worker waits.
    constexpr uint32_t HO_SEND_TRIES = 1000u;
    // Bounds the send to a seated receiver that has not waited, which must not park at all.
    constexpr uint32_t HO_SEATED_TIMEOUT_US = 50000u;
    Atomic<int32_t, Order::RELAXED> g_ho_rn{-99};
    kos_cap_t g_ho_gate = KOS_CAP_NONE;
    void ho_receiver(void*) // caps: done@1, E(WAIT)@2, gate@3
    {
        kos_sem_wait(3);
        char buf[sizeof(EP_MSG)];
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, 2, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        g_ho_rn = kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &o);
        kos_sem_post(CH_DONE);
    }
    int32_t ho_seat(kos::thread::Handle* out)
    {
        kos_cap_grant const caps[] = {{g_done, CH_FULL}, {g_ep, KOS_CAP_WAIT}, {g_ho_gate, CH_FULL}};
        *out = kos::thread::create_caps(ho_receiver, nullptr, "hoR", 12, caps, 3,
                                        KOS_POLICY_FIFO, 0, /*privileged=*/false);
        return out->error();
    }
    void t_endpoint_handout()
    {
        if (kos_endpoint_create(&g_ep) != 0)
        {
            tap::skip("endpoint pool too small");
            return;
        }
        TAP_CHECK(kos_sem_create(0, &g_ho_gate) == 0);
        int32_t const mlen = static_cast<int32_t>(strlen(EP_MSG));
        int32_t const narrowed = kos_cap_narrow(g_ep, EP_HANDOUT_ONLY);
        int32_t const unserved = kos_send(g_ep, EP_MSG, strlen(EP_MSG));
        char buf[sizeof(EP_MSG)];
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, g_ep, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        int32_t const not_receiver =
            kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &o);
        g_ho_rn = -99;
        kos::thread::Handle w;
        int32_t const seat_rc = ho_seat(&w);
        int32_t seated = -99;
        int32_t served = -99;
        if (seat_rc == 0)
        {
            seated = kos_send_timed(g_ep, EP_MSG, strlen(EP_MSG), HO_SEATED_TIMEOUT_US);
            kos_sem_post(g_ho_gate);
            served = -KOS_EAGAIN;
            for (uint32_t i = 0; i < HO_SEND_TRIES and served == -KOS_EAGAIN; i++)
            {
                served = kos_send(g_ep, EP_MSG, strlen(EP_MSG));
                if (served == -KOS_EAGAIN)
                {
                    kos_sleep_ns(1000000ull);
                }
            }
            wait_n(1);
            (void)w.join();
        }
        int32_t const again = kos_send(g_ep, EP_MSG, strlen(EP_MSG));
        int32_t const dropped = kos_cap_narrow(g_ep, KOS_CAP_SIGNAL | KOS_CAP_TRANSFER);
        int32_t const refused = kos_send(g_ep, EP_MSG, strlen(EP_MSG));
        kos::thread::Handle b;
        int32_t const bad_seat = ho_seat(&b);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        kos_sem_destroy(g_ho_gate);
        int32_t const ho_rn = g_ho_rn;
        tap::diag("narrowed %d, unserved %d, recv %d, seat %d, seated %d, served %d, again %d, "
                  "refused %d, seat without the right %d",
                  static_cast<int>(narrowed), static_cast<int>(unserved),
                  static_cast<int>(not_receiver), static_cast<int>(seat_rc),
                  static_cast<int>(seated), static_cast<int>(served), static_cast<int>(again),
                  static_cast<int>(refused), static_cast<int>(bad_seat));
        TAP_CHECK(narrowed == 0 and unserved == -KOS_EAGAIN and not_receiver == -KOS_EACCES);
        TAP_CHECK(seat_rc == 0 and seated == -KOS_EAGAIN and served == mlen and ho_rn == mlen);
        TAP_CHECK(again == -KOS_EAGAIN and dropped == 0 and refused == -KOS_ECONNREFUSED);
        TAP_CHECK(bad_seat == -KOS_EACCES);
    }

    // --- A parked sender woken by the last receiver leaving, the handout right kept ----------
    // Main's creator cap is the only receiver; a worker parks sending, and main narrows WAIT
    // away while keeping the handout right. The narrow drops the last receiver as a close
    // would, and the parked sender is answered what a new caller would be: -KOS_EAGAIN.
    Atomic<int32_t, Order::RELAXED> g_hp_rc{-99};
    void hp_sender(void*) // caps: done@1, E(SIGNAL)@2
    {
        g_hp_rc = kos_send(2, EP_MSG, strlen(EP_MSG)); // parks: a receiver exists, none waits
        kos_sem_post(CH_DONE);
    }
    void t_endpoint_handout_parked()
    {
        if (kos_endpoint_create(&g_ep) != 0)
        {
            tap::skip("endpoint pool too small");
            return;
        }
        g_hp_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto w = kos::thread::create_caps(hp_sender, nullptr, "hpS", 12, caps, 2,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        kos_sleep_ns(3000000ull); // let the sender park behind main's WAIT
        TAP_CHECK(kos_cap_narrow(g_ep, EP_HANDOUT_ONLY) == 0);
        wait_n(1);
        int32_t const hp_rc = g_hp_rc;
        TAP_CHECK(hp_rc == -KOS_EAGAIN);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    // --- A receiver taking nothing, against a writer offering the rest again ---------------
    // Both threads on one core. The writer starts below the receiver, so it first runs with
    // the receiver parked, and then rises above it. A rendezvous consumes the receive it
    // completes, a zero-byte one included, so every offer after the first waits for the
    // receiver to run and receive again. A send answered before the receiver posted its next
    // receive is a spin that would starve it.
    constexpr int ZA_ZEROS = 64;
    constexpr uint8_t ZA_RECEIVER_PRIO = 12;
    constexpr uint8_t ZA_WRITER_START_PRIO = 11;
    constexpr uint8_t ZA_WRITER_PRIO = 14;
    char const ZA_MSG[] = "zero-accept line\n";
    Atomic<int, Order::RELAXED> g_za_posted{0};
    Atomic<int, Order::RELAXED> g_za_sends{0};
    Atomic<int, Order::RELAXED> g_za_spun{0};
    Atomic<int32_t, Order::RELAXED> g_za_send_rc{0};
    Atomic<int32_t, Order::RELAXED> g_za_recv_rc{0};
    Atomic<int, Order::RELAXED> g_za_got{0};
    Atomic<int, Order::RELAXED> g_za_raised{-99};
    char g_za_buf[sizeof(ZA_MSG)];

    // The lowest core of this task's set, or no placement on one core.
    uint32_t za_core_mask()
    {
#if KICKOS_KERNEL_CORES > 1
        uintptr_t const cores = kos_sched_probe(KOS_SCHED_OP_TASK_CORES);
        uint32_t const set = static_cast<uint32_t>(cores);
        if (static_cast<intptr_t>(cores) <= 0 or set == 0)
        {
            return 1u;
        }
        return set & (0u - set);
#else
        return 0;
#endif
    }

    void za_receiver(void*) // caps: done@1, E(WAIT)@2
    {
        size_t const n = strlen(ZA_MSG);
        size_t got = 0;
        int32_t rc = 0;
        for (int i = 0; i <= ZA_ZEROS + 1 and got < n; i++)
        {
            size_t cap = 0;
            if (i == ZA_ZEROS)
            {
                cap = 2;
            }
            else if (i > ZA_ZEROS)
            {
                cap = sizeof(g_za_buf) - got;
            }
            g_za_posted = g_za_posted.load() + 1;
            struct kos_reply_recv_opts o;
            kos_reply_recv_opts_init(&o, 2, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
            rc = kos_reply_recv(KOS_CAP_NONE, g_za_buf + got, kos_call_lens_pack(0, cap), &o);
            if (rc < 0)
            {
                break;
            }
            got += static_cast<size_t>(rc);
        }
        g_za_recv_rc = rc;
        g_za_got = static_cast<int>(got);
        kos_sem_post(CH_DONE);
    }

    void za_writer(void*) // caps: done@1, E(SIGNAL)@2
    {
        size_t const n = strlen(ZA_MSG);
        size_t sent = 0;
        int sends = 0;
        g_za_raised = kos_thread_set_priority(ZA_WRITER_PRIO);
        while (sent < n)
        {
            int32_t const rc = kos_send(2, ZA_MSG + sent, n - sent);
            sends++;
            if (rc < 0)
            {
                g_za_send_rc = rc;
                break;
            }
            if (sends > g_za_posted.load())
            {
                g_za_spun = sends - g_za_posted.load();
                break;
            }
            sent += static_cast<size_t>(rc);
        }
        g_za_sends = sends;
        kos_sem_post(CH_DONE);
    }

    void t_endpoint_zero_accept()
    {
        if (kos_endpoint_create(&g_ep) != 0)
        {
            tap::skip("endpoint pool too small");
            return;
        }
        g_za_posted = 0;
        g_za_sends = 0;
        g_za_spun = 0;
        g_za_send_rc = 0;
        g_za_recv_rc = 0;
        g_za_got = 0;
        g_za_raised = -99;
        memset(g_za_buf, 0, sizeof(g_za_buf));
        uint32_t const core = za_core_mask();
        kos_cap_grant const rcaps[] = {{g_done, CH_FULL}, {g_ep, KOS_CAP_WAIT}};
        kos_cap_grant const wcaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto r = kos::thread::create_caps(za_receiver, nullptr, "zaR", ZA_RECEIVER_PRIO, rcaps,
                                          2, KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr,
                                          0, 0, nullptr, KOS_TASK_NONE, nullptr, 0, core);
        if (not r.valid())
        {
            TAP_CHECK(kos_handle_close(g_ep) == 0);
            tap::skip("thread pool too small for the receiver");
            return;
        }
        auto w = kos::thread::create_caps(za_writer, nullptr, "zaW", ZA_WRITER_START_PRIO,
                                          wcaps, 2, KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                          nullptr, 0, 0, nullptr, KOS_TASK_NONE, nullptr, 0, core);
        int done = 2;
        if (not w.valid())
        {
            done = 1;
        }
        wait_n(done);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        size_t const n = strlen(ZA_MSG);
        int const sends = g_za_sends;
        int const posted = g_za_posted;
        int const spun = g_za_spun;
        int const got = g_za_got;
        int const raised = g_za_raised;
        tap::diag("core mask 0x%x, raise %d: %d send(s) against %d receive(s), %d past them; "
                  "send rc %d, recv rc %d, %d of %u byte(s) received",
                  static_cast<unsigned>(core), raised, sends, posted, spun,
                  static_cast<int>(g_za_send_rc.load()), static_cast<int>(g_za_recv_rc.load()),
                  got, static_cast<unsigned>(n));
        TAP_CHECK(w.valid() and raised == 0);
        TAP_CHECK(spun == 0 and sends == ZA_ZEROS + 2 and posted == sends);
        TAP_CHECK(got == static_cast<int>(n) and memcmp(g_za_buf, ZA_MSG, n) == 0);
    }

    // --- Timed send: the deadline expires with a live endpoint and nobody in recv ------
    // Main keeps its WAIT cap for the whole arm, so recv_holders stays 1 and no EPIPE can
    // fire: the only thing missing is a parked receiver. The worker's report rides an
    // UNTIMED send on the SAME endpoint, so the report arriving proves that form still parks.
    constexpr uint32_t EP_SEND_TIMEOUT_US = 4000;
    // Set by the timed arms' workers once their timed call has returned. Main acts on the
    // endpoint only after it, so the deadline has fired whatever the host did to either thread.
    Atomic<int, Order::RELAXED> g_ep_timed_returned{0};

    // Whether g_ep_timed_returned was set within STALL_TOLERANT_US.
    bool ep_timed_awaited()
    {
        uint64_t const give_up = kos_clock_now() + STALL_TOLERANT_US * 1000ull;
        while (g_ep_timed_returned.load() == 0 and kos_clock_now() < give_up)
        {
            kos_sleep_ns(1000000ull);
        }
        return g_ep_timed_returned.load() != 0;
    }

    // Closing main's WAIT refuses the worker's report, so its own wait ends as well.
    void ep_timed_abandon()
    {
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        wait_n(1);
        tap::fail("the timed call never returned");
    }
    struct EpTimedSend
    {
        int32_t rc;
        uint32_t waited_us;
    };
    void ep_timed_worker(void*) // caps: done@1, E(SIGNAL)@2
    {
        EpTimedSend r;
        uint64_t const t0 = kos_clock_now();
        r.rc = static_cast<int32_t>(
            kos_send_timed(2, EP_MSG, strlen(EP_MSG), EP_SEND_TIMEOUT_US));
        r.waited_us = static_cast<uint32_t>((kos_clock_now() - t0) / 1000ull);
        g_ep_timed_returned = 1;
        (void)kos_send(2, &r, sizeof(r)); // untimed: parks until main's recv, long after
        kos_sem_post(CH_DONE);
    }
    void t_endpoint_send_timeout()
    {
        g_ep_timed_returned = 0;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto w = kos::thread::create_caps(ep_timed_worker, nullptr, "eptm", 12, caps, 2,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        if (not ep_timed_awaited())
        {
            ep_timed_abandon();
            return;
        }
        EpTimedSend r;
        memset(&r, 0, sizeof(r));
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, g_ep, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        int32_t const n =
            kos_reply_recv(KOS_CAP_NONE, &r, kos_call_lens_pack(0, sizeof(r)), &o);
        TAP_CHECK(n == static_cast<int32_t>(sizeof(r))); // the untimed report parked and landed
        TAP_CHECK(r.rc == -KOS_ETIMEDOUT);            // expired, and no bytes crossed
        // Both clock reads bracket the syscall, so this cannot pass on a deadline the
        // kernel fired immediately.
        TAP_CHECK(r.waited_us >= EP_SEND_TIMEOUT_US);
        wait_n(1);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    // The three timed-call arms share one deadline, and each waits for its caller's call to
    // return before it acts. Main keeps its WAIT cap for the whole of each, so recv_holders
    // stays 1 and no EPIPE can be mistaken for an expiry.
    constexpr uint32_t EP_CALL_TIMEOUT_US = 4000;
    // 20x that deadline, for the one-core arm whose server cannot see the call return.
    constexpr uint64_t EP_CALL_TIMEOUT_WAIT_NS = 80000000ull;
    constexpr uint64_t EP_CALL_SETTLE_NS = 3000000ull; // long enough for the caller to park
    // The reply-wait arm needs the OPPOSITE ordering: the server must pop the caller BEFORE
    // its deadline fires. A host that stalls main past it makes the arm vacuous.
    constexpr uint32_t EP_CALL_REPLY_TIMEOUT_US = 60000;
    // A deadline no arm here can reach: it is on a recv that pops an ALREADY-parked peer,
    // so it never arms, and it doubles as the witness that the kernel leaves the input word
    // alone.
    constexpr uint32_t EP_RECV_GENEROUS_US = 200000;

    // --- Timed call: the deadline expires while parked on send_waiters ------------------
    // The endpoint HAS a conventional server (main's warm-up recv seats ep->server) but
    // nobody is in recv when the call lands, so the caller parks as CALL_SEND_WAIT and the
    // unwind has a real D2 boost to revert rather than the null-server shortcut.
    void ep_call_pending_worker(void*) // caps: done@1, E(SIGNAL)@2
    {
        char warm[1] = {0};
        kos_send(2, warm, sizeof(warm)); // main's warm-up recv takes this and seats ep->server
        char buf[8] = {0};
        EpTimedSend r;
        uint64_t const t0 = kos_clock_now();
        r.rc = kos_call_timed(2, buf, 4, sizeof(buf), EP_CALL_TIMEOUT_US);
        r.waited_us = static_cast<uint32_t>((kos_clock_now() - t0) / 1000ull);
        g_ep_timed_returned = 1;
        (void)kos_send(2, &r, sizeof(r)); // untimed: parks until main's recv, long after
        kos_sem_post(CH_DONE);
    }
    void t_call_timeout_pending()
    {
        g_ep_timed_returned = 0;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto w = kos::thread::create_caps(ep_call_pending_worker, nullptr, "cltp", 12, caps, 2,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        char warm[1] = {0};
        struct kos_reply_recv_opts warm_o;
        kos_reply_recv_opts_init(&warm_o, g_ep, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, warm, kos_call_lens_pack(0, sizeof(warm)),
                                 &warm_o)
                  == 1);
        if (not ep_timed_awaited())
        {
            ep_timed_abandon();
            return;
        }
        EpTimedSend r;
        memset(&r, 0, sizeof(r));
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, g_ep, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        int32_t const n =
            kos_reply_recv(KOS_CAP_NONE, &r, kos_call_lens_pack(0, sizeof(r)), &o);
        TAP_CHECK(n == static_cast<int32_t>(sizeof(r))); // the untimed report parked and landed
        TAP_CHECK(r.rc == -KOS_ETIMEDOUT);            // expired on send_waiters, never taken
        // Both clock reads bracket the syscall, so this cannot pass on a deadline the
        // kernel fired immediately.
        TAP_CHECK(r.waited_us >= EP_CALL_TIMEOUT_US);
        wait_n(1);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    // --- Timed call: the deadline expires in CALL_REPLY_WAIT, reached by the slow path ---
    // The caller parks on send_waiters first, and main's info-bearing recv then MIGRATES it
    // onto main's reply_waiters. That migration is a park-to-park move and not an unpark, so
    // the deadline armed once at the call must survive it; were the cancel back in
    // wq_pop_highest this caller would park forever and the arm would HANG. Staged on the
    // fast path every rc, duration and cap assertion still passes; only the caller's mark,
    // read by main before its recv, separates them.
    Atomic<int, Order::RELAXED> g_cltr_calling{0};
    void ep_call_reply_worker(void*) // caps: done@1, E(SIGNAL)@2, go(SIGNAL)@3
    {
        char buf[8] = {0};
        EpTimedSend r;
        // Posted before the mark: a main that could preempt this thread reads the mark unset.
        kos_sem_post(3);
        g_cltr_calling = 1;
        uint64_t const t0 = kos_clock_now();
        r.rc = kos_call_timed(2, buf, 4, sizeof(buf), EP_CALL_REPLY_TIMEOUT_US);
        r.waited_us = static_cast<uint32_t>((kos_clock_now() - t0) / 1000ull);
        g_ep_timed_returned = 1;
        (void)kos_send(2, &r, sizeof(r));
        kos_sem_post(CH_DONE);
    }
    void t_call_timeout_reply()
    {
        g_ep_timed_returned = 0;
        g_cltr_calling = 0;
        kos_cap_t go = KOS_CAP_NONE;
        kos_cap_t reply_cap = KOS_CAP_NONE;
        kos::thread::Handle w;
        ArmHold hold;
        hold.thread(&w);
        hold.cap(&g_ep);
        hold.cap(&go);
        hold.cap(&reply_cap);
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        TAP_CHECK(kos_sem_create(0, &go) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}, {go, KOS_CAP_SIGNAL}};
        // Above main on main's core: main resumes only once this caller has parked.
        w = kos::thread::create_caps(ep_call_reply_worker, nullptr, "cltr", TAP_PRIO_PARKS, caps,
                                     3, KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0, 0,
                                     nullptr, KOS_TASK_NONE, nullptr, 0, TAP_PIN_CORE);
        TAP_CHECK(w.valid());
        TAP_CHECK(kos_sem_wait(go) == 0);
        int const caller_first = g_cltr_calling.load();
        char req[8];
        // The caller is already queued. Check that writing receive metadata
        // preserves the input timeout.
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, g_ep, 0, EP_RECV_GENEROUS_US);
        int32_t const got = kos_reply_recv(KOS_CAP_NONE, req,
                                           kos_call_lens_pack(0, sizeof(req)),
                                           &opts); // slow-path pop + migration
        reply_cap = opts.info.reply_cap;
        TAP_CHECK(caller_first == 1);
        if (got == static_cast<int32_t>(sizeof(EpTimedSend)) and reply_cap == KOS_CAP_NONE)
        {
            // The report, not the request: the deadline fired on send_waiters first.
            EpTimedSend early;
            memcpy(&early, req, sizeof(early));
            TAP_CHECK(early.rc == -KOS_ETIMEDOUT);
            TAP_CHECK(early.waited_us >= EP_CALL_REPLY_TIMEOUT_US);
            TAP_SKIP_VACUOUS("the caller's %u us deadline fired before main's recv popped it",
                             static_cast<unsigned>(EP_CALL_REPLY_TIMEOUT_US));
            wait_n(1);
            return;
        }
        TAP_CHECK(got == 4);
        TAP_CHECK(reply_cap != KOS_CAP_NONE); // we hold the reply cap, and never use it
        TAP_CHECK(opts.timeout_us == EP_RECV_GENEROUS_US);
        if (not ep_timed_awaited())
        {
            tap::fail("the timed call never returned");
            return;
        }
        EpTimedSend r;
        memset(&r, 0, sizeof(r));
        struct kos_reply_recv_opts ro;
        kos_reply_recv_opts_init(&ro, g_ep, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        int32_t const n =
            kos_reply_recv(KOS_CAP_NONE, &r, kos_call_lens_pack(0, sizeof(r)), &ro);
        TAP_CHECK(n == static_cast<int32_t>(sizeof(r)));
        TAP_CHECK(r.rc == -KOS_ETIMEDOUT); // the deadline crossed the handoff and fired
        TAP_CHECK(r.waited_us >= EP_CALL_REPLY_TIMEOUT_US);
        // The cap outlives the caller by design, so closing it is still the server's job and
        // must succeed with nobody left to wake.
        TAP_CHECK(hold.close(&reply_cap) == 0);
        wait_n(1);
        TAP_CHECK(hold.close(&g_ep) == 0);
    }

    // --- Reply to a caller that already timed out: -KOS_ESRCH, cap consumed --------------
    // Staged on the FAST path (main is already parked in recv when the call lands), which
    // is the other half of the CALL_REPLY_WAIT unwind: there the deadline is armed straight
    // onto the reply park. The caller reads main's mark before its call, so a slow-path
    // staging fails on the mark instead of duplicating the reply-wait arm.
    Atomic<int, Order::RELAXED> g_rpst_main_receiving{0};
    Atomic<int, Order::RELAXED> g_rpst_caller_saw{-1};
    void ep_reply_stale_worker(void*) // caps: done@1, E(SIGNAL)@2
    {
        char buf[8] = {0};
        EpTimedSend r;
        g_rpst_caller_saw = g_rpst_main_receiving.load();
        uint64_t const t0 = kos_clock_now();
        r.rc = kos_call_timed(2, buf, 4, sizeof(buf), EP_CALL_TIMEOUT_US);
        r.waited_us = static_cast<uint32_t>((kos_clock_now() - t0) / 1000ull);
        g_ep_timed_returned = 1;
        (void)kos_send(2, &r, sizeof(r));
        kos_sem_post(CH_DONE);
    }
    void t_reply_stale_caller()
    {
        g_ep_timed_returned = 0;
        g_rpst_main_receiving = 0;
        g_rpst_caller_saw = -1;
        kos_cap_t reply_cap = KOS_CAP_NONE;
        kos::thread::Handle w;
        ArmHold hold;
        hold.thread(&w);
        hold.cap(&g_ep);
        hold.cap(&reply_cap);
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        // Below main on main's core: the caller first runs once main has parked in its recv.
        w = kos::thread::create_caps(ep_reply_stale_worker, nullptr, "rpst", TAP_PRIO_AFTER, caps,
                                     2, KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0, 0,
                                     nullptr, KOS_TASK_NONE, nullptr, 0, TAP_PIN_CORE);
        TAP_CHECK(w.valid());
        char req[8];
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, g_ep, 0, KOS_TIMEOUT_NONE);
        g_rpst_main_receiving = 1;
        int32_t const got =
            kos_reply_recv(KOS_CAP_NONE, req, kos_call_lens_pack(0, sizeof(req)), &opts);
        reply_cap = opts.info.reply_cap;
        TAP_CHECK(g_rpst_caller_saw.load() == 1);
        TAP_CHECK(got == 4 and reply_cap != KOS_CAP_NONE);
        if (not ep_timed_awaited())
        {
            tap::fail("the timed call never returned");
            return;
        }
        char rep[4] = {0};
        // The cap still resolves, but the caller it names left CALL_REPLY_WAIT, so the
        // reply has nowhere to land. It is consumed anyway.
        TAP_CHECK(kos_reply(reply_cap, rep, sizeof(rep)) == -KOS_ESRCH);
        // Consumed exactly once: the slot is empty and its cap-gen rolled, so the handle no
        // longer resolves at all and the second attempt fails EARLIER, on the cap.
        TAP_CHECK(kos_reply(reply_cap, rep, sizeof(rep)) == -KOS_EBADF);
        TAP_CHECK(hold.close(&reply_cap) == -KOS_EBADF);
        EpTimedSend r;
        memset(&r, 0, sizeof(r));
        struct kos_reply_recv_opts opts2;
        kos_reply_recv_opts_init(&opts2, g_ep, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        int32_t const n = kos_reply_recv(KOS_CAP_NONE, &r, kos_call_lens_pack(0, sizeof(r)), &opts2);
        TAP_CHECK(n == static_cast<int32_t>(sizeof(r)));
        TAP_CHECK(r.rc == -KOS_ETIMEDOUT); // expired on the reply park, not on send_waiters
        TAP_CHECK(r.waited_us >= EP_CALL_TIMEOUT_US);
        wait_n(1);
        TAP_CHECK(hold.close(&g_ep) == 0);
    }

    // Time out a call, then wrap its sequence while the first server keeps its
    // reply cap. The caller is now waiting on a second server, so the old server
    // must fail to unlink it even though handle and sequence match.
    // Use a second server because the first has filled its reply-cap budget.
    // Synchronize on a separate endpoint to avoid receiving a loop call as a token.
    char const SR_GOOD[] = "OK!!";
    char const SR_BAD[] = "BAD!";
    // The abandoned call left call_seq at A and the timeout unwind rolled it to A+1, so the
    // caller's k-th further call runs at A+1+k and the packed low byte (A) comes round again
    // at k == 255. The loops below are sized so the LAST call is exactly that one: anything
    // shorter and the sequence test refuses the cap before this arm's guard is consulted.
    constexpr int SR_SEQ_PERIOD = 1 << 8; // KCAP_REPLY_SEQ_BITS
    constexpr int SR_ALIAS_CALL = SR_SEQ_PERIOD - 1;
    void sr_caller(void*) // caps: done@1, lock@2, E1(SIGNAL)@3
    {
        char buf[8] = {0};
        kos_sleep_ns(EP_CALL_SETTLE_NS); // main parks in recv first: the call takes the fastpath
        if (kos_call_timed(3, buf, 4, sizeof(buf), EP_CALL_TIMEOUT_US) == -KOS_ETIMEDOUT)
        {
            log_put('T');
        }
        bool ok = true;
        for (int k = 0; k < SR_ALIAS_CALL; k++)
        {
            memcpy(buf, "req2", 4);
            int32_t const rc = kos_call(3, buf, 4, sizeof(buf));
            if (rc != 4 or memcmp(buf, SR_GOOD, 4) != 0)
            {
                ok = false; // one log entry for the whole loop: 255 of these say nothing
            }
        }
        char c = 'X';
        if (ok)
        {
            c = 'K'; // every call, the aliasing one included, got its own server's bytes
        }
        log_put(c);
        kos_sem_post(CH_DONE);
    }
    void sr_second_server(void*) // caps: done@1, E1(FULL)@2, E2(FULL)@3
    {
        char buf[8];
        kos_sleep_ns(EP_CALL_TIMEOUT_WAIT_NS); // outlast the first call's deadline
        for (int k = 0; k < SR_ALIAS_CALL - 1; k++)
        {
            struct kos_recv_info info = {0, KOS_CAP_NONE};
            struct kos_reply_recv_opts opts;
            kos_reply_recv_opts_init(&opts, 2, 0, KOS_TIMEOUT_NONE);
            kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
            info = opts.info;
            kos_reply(info.reply_cap, SR_GOOD, 4);
        }
        struct kos_recv_info last = {0, KOS_CAP_NONE};
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 2, 0, KOS_TIMEOUT_NONE);
        kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts); // the aliasing call, held unanswered
        last = opts.info;
        kos_send(3, "rdy", 3);                // rendezvous: completes only in main's recv
        // Main runs its reply while we are parked here; the go token releases us.
        struct kos_reply_recv_opts opts2;
        kos_reply_recv_opts_init(&opts2, 3, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts2);
        kos_reply(last.reply_cap, SR_GOOD, 4);
        kos_sem_post(CH_DONE);
    }
    void t_reply_abandoned_cap()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        if (not pool_can_host(2))
        {
            tap::skip("pool too small (2 interdependent workers)");
            return;
        }
        log_reset();
        kos_cap_t ep2 = KOS_CAP_NONE;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        TAP_CHECK(kos_endpoint_create(&ep2) == 0);
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_ep, CH_FULL}, {ep2, CH_FULL}};
        auto cl = kos::thread::create_caps(sr_caller, nullptr, "srC", 12, ccaps, 3);
        auto s2 = kos::thread::create_caps(sr_second_server, nullptr, "srS", 10, scaps, 3);
        TAP_CHECK(cl.valid() and s2.valid());
        char req[8];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, g_ep, 0, KOS_TIMEOUT_NONE);
        int32_t const got = kos_reply_recv(KOS_CAP_NONE, req, kos_call_lens_pack(0, sizeof(req)), &opts); // first call
        info = opts.info;
        TAP_CHECK(got == 4 and info.reply_cap != KOS_CAP_NONE);
        // Never replied, and never touched again until the token arrives: the deadline
        // expires under us and this cap is abandoned for the rest of the arm.
        char tok[8];
        struct kos_reply_recv_opts opts2;
        kos_reply_recv_opts_init(&opts2, ep2, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        int32_t const rdy = kos_reply_recv(KOS_CAP_NONE, tok, kos_call_lens_pack(0, sizeof(tok)), &opts2);
        TAP_CHECK(rdy == 3 and memcmp(tok, "rdy", 3) == 0); // the aliasing call is parked
        // Every resolve test but the reply-waiter unlink now passes on this cap.
        TAP_CHECK(kos_reply(info.reply_cap, SR_BAD, 4) == -KOS_ESRCH);
        // Consumed exactly once even on the refusal, so the handle no longer resolves.
        TAP_CHECK(kos_reply(info.reply_cap, SR_BAD, 4) == -KOS_EBADF);
        TAP_CHECK(kos_handle_close(info.reply_cap) == -KOS_EBADF);
        TAP_CHECK(kos_send(ep2, "go", 2) == 2); // release the second server's reply
        wait_n(2);
        TAP_CHECK(kos_handle_close(ep2) == 0);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(log_eq("TK")); // timed out, then every call answered by its own server
    }

    // --- Timed recv: the deadline expires with nobody sending ---------------------------
    // Runs in main with NO worker: nothing arrives, so the deadline alone is what expires
    // and no cross-domain copy is involved.
    void t_recv_timeout()
    {
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        char buf[8];
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, g_ep, 0, EP_CALL_TIMEOUT_US);
        opts.notify = 0u; // no accepted IRQs; output must stay zero
        uint64_t const t0 = kos_clock_now();
        int32_t const n =
            kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        uint32_t const waited_us = static_cast<uint32_t>((kos_clock_now() - t0) / 1000ull);
        TAP_CHECK(n == -KOS_ETIMEDOUT); // expired, and no bytes arrived
        TAP_CHECK(waited_us >= EP_CALL_TIMEOUT_US);
        TAP_CHECK(opts.timeout_us == EP_CALL_TIMEOUT_US); // an input, never written back
        // With no accepted IRQs, notify must remain zero.
        TAP_CHECK(opts.notify == 0u);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    // --- Malformed arguments to the timed forms -----------------------------------------
    // Every refusal here is decided before anything parks or copies, so main runs the whole
    // arm alone. The EFAULT half needs a pointer the caller does not own, which a privileged
    // caller can never present: it lives in t_endpoint_bound.
    void t_timed_arg_refusals()
    {
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        char buf[8];
        uintptr_t const lens = kos_call_lens_pack(0, sizeof(buf));
        // The kernel must reject missing opts; the stub must not hide this check.
        TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, buf, lens, nullptr) == -KOS_EINVAL);
        // Misaligned opts must return EINVAL before any privileged word access.
        alignas(alignof(struct kos_reply_recv_opts))
            unsigned char raw[sizeof(struct kos_reply_recv_opts) + alignof(uint32_t)];
        memset(raw, 0, sizeof(raw));
        struct kos_reply_recv_opts* const skewed =
            reinterpret_cast<struct kos_reply_recv_opts*>(raw + 1);
        TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, buf, lens, skewed) == -KOS_EINVAL);
        // An undefined flag bit is refused and never masked: the field is an INPUT in a
        // struct callers declare, so a word nobody set must not read as a posture nobody
        // asked for. Refused before the park, so nothing here can block.
        struct kos_reply_recv_opts bad;
        kos_reply_recv_opts_init(&bad, g_ep, KOS_RECV_NO_INFO | 0x2u, EP_CALL_TIMEOUT_US);
        TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, buf, lens, &bad) == -KOS_EINVAL);
        bad.flags = 0xFFFFFFFFu;
        TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, buf, lens, &bad) == -KOS_EINVAL);
        // The timed call packs both lengths into one argument word and SATURATES rather than
        // masking. A masked 512 would arrive as 0 and become a silent zero-length call; the
        // saturated value is still above KOS_EP_MSG_MAX, so the oversize refusal survives it.
        TAP_CHECK(kos_call_timed(g_ep, buf, KOS_EP_MSG_MAX + 1, sizeof(buf),
                                 EP_CALL_TIMEOUT_US)
                  == -KOS_EINVAL);
        TAP_CHECK(kos_call_timed(g_ep, buf, 512, sizeof(buf), EP_CALL_TIMEOUT_US)
                  == -KOS_EINVAL);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    // Test oversized reply and receive lengths separately. With no sender or
    // accepted IRQ, ETIMEDOUT proves the lengths were clamped rather than rejected.
    void t_reply_recv_lens_clamp()
    {
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        // Provide KOS_EP_MSG_MAX bytes so buffer validation does not hide the clamp.
        char buf[KOS_EP_MSG_MAX];
        memset(buf, 0, sizeof(buf));
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, g_ep, 0, EP_CALL_TIMEOUT_US);
        TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, 512), &opts)
                  == -KOS_ETIMEDOUT);
        // Reply length is clamped even when KOS_CAP_NONE skips the reply.
        kos_reply_recv_opts_init(&opts, g_ep, 0, EP_CALL_TIMEOUT_US);
        TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(512, sizeof(buf)),
                                 &opts)
                  == -KOS_ETIMEDOUT);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }

    uint64_t g_call_unit = 1000000ull;

    // --- Timed call: the expiry unwind reverts the D2 boost ------------------------------
    // The only arm that can tell whether endpoint_wait_timeout still calls set_prio on the
    // WAIT_EP_SEND branch: a lost boost changes no return code anywhere else. Staging, in
    // units of mtx_time_unit(): the server takes main's plain send first, seating it as the
    // endpoint's conventional server, then busy-spins so the caller must park on
    // send_waiters. The caller (high) wakes mid-spin and calls with a deadline, the spoiler
    // (medium) after that, held off only by the boost. 'u' before 'm' is the boost holding.
    // Three spans decide whether this arm has a subject at all: the call must land inside
    // the first spin (that is what parks it and seats the boost), the spoiler must fall due
    // before that spin ends, and the deadline must expire inside the second spin (that is
    // where a revert is visible), which lasts until the spoiler has run. A spoiler due before
    // the call is still a subject: its sleep is armed after the caller's and ends later, so the
    // caller always runs and parks first.
    Window g_ctr_spin1;
    Window g_ctr_spin2;
    Atomic<uint32_t, Order::RELAXED> g_ctr_call_at{0};
    Atomic<uint32_t, Order::RELAXED> g_ctr_deadline{0};
    Atomic<uint32_t, Order::RELAXED> g_ctr_spoiler_due{0};
    // Set by the spoiler once it has run. The second span lasts until then rather than for a
    // fixed time: the revert is taken when the deadline's interrupt is, which a loaded host can
    // deliver after any fixed span. CTR_GIVE_UP_NS only turns a revert that never comes, the
    // defect, into a failure instead of a hang.
    Atomic<uint32_t, Order::RELAXED> g_ctr_spoiler_ran{0};
    constexpr uint64_t CTR_GIVE_UP_NS = 5000000000ull;
    void ctr_server(void*) // caps: done@1, lock@2, E(WAIT)@3
    {
        char buf[16];
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 3, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts); // takes main's plain send; ep->server = us
        log_put('a');
        window_open(g_ctr_spin1);
        mtx_spin(g_call_unit * 6);  // the caller wakes and D2-boosts us inside this
        window_close(g_ctr_spin1);
        log_put('u');
        window_open(g_ctr_spin2);
        uint64_t const spin2_from = kos_clock_now();
        while (g_ctr_spoiler_ran.load() == 0u and kos_clock_now() - spin2_from < CTR_GIVE_UP_NS)
        {
        }
        window_close(g_ctr_spin2);
        log_put('z');
        kos_sem_post(CH_DONE);
    }
    void ctr_caller(void*) // caps: done@1, lock@2, E(SIGNAL)@3
    {
        char buf[8] = {0};
        kos_sleep_ns(g_call_unit * 2); // wake mid-spin: the server is not in recv, so we park
        uint32_t const deadline_us = static_cast<uint32_t>((g_call_unit * 8ull) / 1000ull);
        uint32_t const at = stamp_now();
        g_ctr_call_at = at;
        g_ctr_deadline = at + (deadline_us * 1000u);
        int32_t const rc = kos_call_timed(3, buf, 4, sizeof(buf), deadline_us);
        char c = 'X';
        if (rc == -KOS_ETIMEDOUT)
        {
            c = 'c';
        }
        log_put(c);
        kos_sem_post(CH_DONE);
    }
    void ctr_spoiler(void*) // caps: done@1, lock@2 (medium prio)
    {
        g_ctr_spoiler_due = stamp_due(g_call_unit * 4);
        kos_sleep_ns(g_call_unit * 4); // ready while the server is boosted, so it must wait
        log_put('m');
        g_ctr_spoiler_ran = 1;
        kos_sem_post(CH_DONE);
    }
    void t_call_timeout_revert()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        // Ask before spawning: the three workers wait on each other, so a partial set
        // cannot be drained and a guard after the spawns would hang rather than skip.
        if (not pool_can_host(3))
        {
            tap::skip("pool too small (3 interdependent workers)");
            return;
        }
        log_reset();
        g_call_unit = mtx_time_unit();
        window_reset(g_ctr_spin1);
        window_reset(g_ctr_spin2);
        g_ctr_call_at = 0;
        g_ctr_deadline = 0;
        g_ctr_spoiler_due = 0;
        g_ctr_spoiler_ran = 0;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto sv = kos::thread::create_caps(ctr_server, nullptr, "ctrS", 8, scaps, 3);
        auto cl = kos::thread::create_caps(ctr_caller, nullptr, "ctrC", 20, ccaps, 3);
        auto sp = kos::thread::create_caps(ctr_spoiler, nullptr, "ctrM", 12, mcaps, 2);
        TAP_CHECK(sv.valid() and cl.valid() and sp.valid());
        char warm[4] = {0};
        kos_send(g_ep, warm, 4); // parks main, which is what lets the workers start
        wait_n(3);
        uint32_t const ctr_call_at = g_ctr_call_at;
        uint32_t const ctr_deadline = g_ctr_deadline;
        uint32_t const ctr_spoiler_due = g_ctr_spoiler_due;
        if (not window_held(g_ctr_spin1, ctr_call_at))
        {
            kos_handle_close(g_ep);
            skip_window_lost("the timed call landing inside the server's first spin, which is "
                             "what parks it on send_waiters and seats the D2 boost",
                             g_ctr_spin1, ctr_call_at);
            return;
        }
        if (not window_held(g_ctr_spin2, ctr_deadline))
        {
            kos_handle_close(g_ep);
            skip_window_lost("the call deadline expiring inside the server's second spin, "
                             "which is the only place the revert is observable",
                             g_ctr_spin2, ctr_deadline);
            return;
        }
        if (not window_reached(g_ctr_spin1, ctr_spoiler_due))
        {
            kos_handle_close(g_ep);
            skip_window_lost("the spoiler falling due before the server's boosted first spin "
                             "ends, which is what the boost has to hold off",
                             g_ctr_spin1, ctr_spoiler_due);
            return;
        }
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(count('c') == 1 and count('X') == 0); // the call expired, it did not bounce
        TAP_CHECK(count('a') == 1 and count('u') == 1 and count('m') == 1 and count('z') == 1);
        TAP_CHECK(nth('u', 1) < nth('m', 1)); // BOOST held while the caller was parked
        TAP_CHECK(nth('m', 1) < nth('z', 1)); // REVERT: the unwind put the server back at base
    }

    // An info-less receive rejects a high-priority call with ENOSYS and removes
    // its donation. Check u before m while boosted, then m before z after rejection.
    // Stage both plain sends through the filler so the second receive sees only
    // the call, rejects it, and parks. main must not add another sender.
    Atomic<int32_t, Order::RELAXED> g_ci_rc{-99};
    kos_cap_t g_ci_bounced = KOS_CAP_NONE; // caller -> filler: the call has bounced
    void ci_server(void*) // caps: done@1, lock@2, E(WAIT)@3, stage@4
    {
        char buf[16];
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 3, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        // Receive the filler's first send and register this server.
        kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        log_put('a');
        // Only now is the caller released: the D2 boost is conditional on ep->server, which
        // recv#1 above is what seats, so a caller that ran first would boost nothing.
        kos_sem_post(4);
        mtx_spin(g_call_unit * 4);              // the caller D2-boosted us inside this
        log_put('u');
        struct kos_reply_recv_opts opts2;
        kos_reply_recv_opts_init(&opts2, 3, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        // Reject the queued call, remove its donation, then park.
        kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts2);
        log_put('z');                          // reached at base prio: spoiler ran first IFF we reverted
        kos_sem_post(CH_DONE);
    }
    void ci_caller(void*) // caps: done@1, lock@2, E(SIGNAL)@3, stage@4, bounced@5
    {
        char buf[8] = {0};
        kos_sem_wait(4);                       // the server is seated
        kos_sem_post(4);                       // the spoiler is ready from here, and we outrank it
        g_ci_rc = kos_call(3, buf, 4, sizeof(buf));
        log_put('c');
        // The reject and the park are one masked window, so the bounce returning here is
        // the proof recv#2 is already on recv_waiters.
        kos_sem_post(5);
        kos_sem_post(CH_DONE);
    }
    void ci_spoiler(void*) // caps: done@1, lock@2, stage@3 (medium prio)
    {
        // Index 3, not the 4 its posters use: holding no endpoint cap shifts the same
        // semaphore one slot down. The wrong index returns at once and this logs first,
        // which reads as a lost boost.
        kos_sem_wait(3);
        log_put('m');
        kos_sem_post(CH_DONE);
    }
    void ci_filler(void*) // caps: done@1, lock@2, E(SIGNAL)@3, bounced@4
    {
        char b[4] = {0};
        kos_send(3, b, 4);                     // the only sender recv#1 can see
        kos_sem_wait(4);
        kos_send(3, b, 4);                     // wakes the parked recv#2
        kos_sem_post(CH_DONE);
    }
    void t_call_infoless_revert()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        // Ask the pool BEFORE spawning anything: the four workers are mutually dependent, so
        // a partial set cannot be drained. Guarding after the spawns HANGS, it does not skip.
        if (not pool_can_host(4))
        {
            tap::skip("pool too small (4 interdependent workers)");
            return;
        }
        log_reset();
        g_call_unit = mtx_time_unit();
        g_ci_rc = -99;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        TAP_CHECK(kos_sem_create(0, &g_gate) == 0);
        // Its own semaphore, not a third post on g_gate: the filler outranks the spoiler,
        // so a shared gate would hand the caller's pre-call post to the filler and its
        // second send would be parked before recv#2 ever ran.
        TAP_CHECK(kos_sem_create(0, &g_ci_bounced) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY},
                                 {g_gate, CH_FULL}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY},
                                 {g_gate, CH_FULL}, {g_ci_bounced, CH_FULL}};
        kos_cap_grant fcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY},
                                 {g_ci_bounced, CH_FULL}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_gate, CH_FULL}};
        auto sv = kos::thread::create_caps(ci_server, nullptr, "ciS", 8, scaps, 4);
        auto cl = kos::thread::create_caps(ci_caller, nullptr, "ciC", 20, ccaps, 5);
        auto sp = kos::thread::create_caps(ci_spoiler, nullptr, "ciM", 12, mcaps, 3);
        // Above the spoiler: its second send is what wakes the parked server, and that wake
        // must land while the spoiler is still only ready. Below the spoiler, 'm' is logged
        // before the server is woken at all and the revert check below passes either way.
        auto fl = kos::thread::create_caps(ci_filler, nullptr, "ciF", 14, fcaps, 4);
        // The probe above just held four slots and four stacks, so a failure now is a pool
        // bug, not a small board.
        TAP_CHECK(sv.valid() and cl.valid() and sp.valid() and fl.valid());
        wait_n(4);
        TAP_CHECK(kos_handle_close(g_ci_bounced) == 0);
        TAP_CHECK(kos_handle_close(g_gate) == 0);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        int32_t const ci_rc = g_ci_rc;
        TAP_CHECK(ci_rc == -KOS_ENOTSUP); // the call bounced off the info-less receiver
        TAP_CHECK(count('a') == 1 and count('u') == 1 and count('m') == 1 and count('z') == 1);
        TAP_CHECK(nth('u', 1) < nth('m', 1)); // BOOST held: boosted server outran the spoiler's wake
        TAP_CHECK(nth('m', 1) < nth('z', 1)); // REVERT: server back at base, spoiler ran before it resumed
    }

    // --- Call/reply: close-instead-of-reply EPIPEs the caller and yields to it -----
    // A low server takes a high caller's call (D1-boosted to the caller's prio), then closes
    // the reply cap instead of replying. The close arm must (a) wake the caller -KOS_EPIPE
    // and (b) deflate the server BEFORE waking, so the higher caller runs before the server
    // proceeds: 'c' strictly before 's'.
    Atomic<int32_t, Order::RELAXED> g_cc_rc{-99};
    void cc_server(void*) // caps: done@1, lock@2, E(WAIT)@3
    {
        char buf[16];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 3, 0, KOS_TIMEOUT_NONE);
        // Accept the call, inherit its priority, and obtain a reply cap.
        kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        info = opts.info;
        kos_handle_close(info.reply_cap);     // close instead of reply: EPIPE the caller + deflate us
        log_put('s');                         // server proceeds: must be AFTER the caller ran
        kos_sem_post(CH_DONE);
    }
    void cc_caller(void*) // caps: done@1, lock@2, E(SIGNAL)@3
    {
        char buf[8] = {0};
        kos_sleep_ns(3000000ull);             // let the server park in recv (fast-path call)
        g_cc_rc = kos_call(3, buf, 4, sizeof(buf));
        log_put('c');
        kos_sem_post(CH_DONE);
    }
    void t_call_close_reply()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        log_reset();
        g_cc_rc = -99;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto sv = kos::thread::create_caps(cc_server, nullptr, "ccS", 8, scaps, 3);
        auto cl = kos::thread::create_caps(cc_caller, nullptr, "ccC", 20, ccaps, 3);
        if (not sv.valid() or not cl.valid())
        {
            int n = 0;
            if (sv.valid()) { n++; }
            if (cl.valid()) { n++; }
            wait_n(n);
            kos_handle_close(g_ep);
            tap::skip("pool too small");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        int32_t const cc_rc = g_cc_rc;
        TAP_CHECK(cc_rc == -KOS_EPIPE);   // (a) caller woken with EPIPE, not a byte count
        TAP_CHECK(nth('c', 1) < nth('s', 1)); // (b) caller ran before the server proceeded
    }

    // --- Call/reply: happy path: request delivered, reply returned in-place ---
    // A server recvs the request (info-bearing), records it, and replies a known payload;
    // the caller's kos_call returns the reply byte count and the reply OVERWRITES its send
    // buffer (in-place). Both paths are covered: (A) server parked in recv first (fastpath),
    // (B) caller parked in SEND_WAIT first, server recvs later (slowpath).
    Atomic<int32_t, Order::RELAXED> g_echo_reqn{-99}; // request bytes the server observed
    char g_echo_reqbuf[8];           // request content the server observed
    Atomic<int32_t, Order::RELAXED> g_echo_rc{-99};   // caller's kos_call return
    char g_echo_rplbuf[8];           // reply content the caller received in-place
    void echo_server(void*)          // caps: done@1, E(WAIT)@2
    {
        char buf[16];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 2, 0, KOS_TIMEOUT_NONE);
        int32_t n = kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        info = opts.info;
        g_echo_reqn = n;
        if (n > 0)
        {
            size_t k = static_cast<size_t>(n);
            if (k > sizeof(g_echo_reqbuf)) { k = sizeof(g_echo_reqbuf); }
            memcpy(g_echo_reqbuf, buf, k);
        }
        if (info.reply_cap != KOS_CAP_NONE)
        {
            char rpl[8];
            memcpy(rpl, "pong!", 5); // reply from the server's OWN stack (unpriv-readable)
            kos_reply(info.reply_cap, rpl, 5);
        }
        kos_sem_post(CH_DONE);
    }
    void echo_caller(void*) // caps: done@1, E(SIGNAL)@2
    {
        char buf[16];
        memcpy(buf, "ping", 4);
        g_echo_rc = kos_call(2, buf, 4, sizeof(buf)); // reply lands back in buf
        if (g_echo_rc > 0)
        {
            size_t k = static_cast<size_t>(g_echo_rc);
            if (k > sizeof(g_echo_rplbuf)) { k = sizeof(g_echo_rplbuf); }
            memcpy(g_echo_rplbuf, buf, k);
        }
        kos_sem_post(CH_DONE);
    }
    // Two transactions through reply-receive: first receive only, then send
    // the first reply while receiving the second request.
    Atomic<int32_t, Order::RELAXED> g_frr_served{-99}; // requests the fused loop served
    Atomic<int32_t, Order::RELAXED> g_frr_ok{-99};     // round trips the caller got right
    void frr_server(void*)                             // caps: done@1, E(WAIT)@2
    {
        char buf[16];
        struct kos_reply_recv_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.ep = 2;
        opts.timeout_us = KOS_TIMEOUT_NONE;
        kos_cap_t reply_cap = KOS_CAP_NONE;
        int served = 0;
        while (served < 2)
        {
            int32_t const n =
                kos_reply_recv(reply_cap, buf, kos_call_lens_pack(5, sizeof(buf)), &opts);
            reply_cap = KOS_CAP_NONE;
            if (n != 4 or memcmp(buf, "ping", 4) != 0)
            {
                break;
            }
            served++;
            memcpy(buf, "pong!", 5);
            reply_cap = opts.info.reply_cap;
            if (reply_cap == KOS_CAP_NONE)
            {
                break;
            }
        }
        // Send the final reply without receiving again.
        if (reply_cap != KOS_CAP_NONE)
        {
            kos_reply(reply_cap, buf, 5);
        }
        g_frr_served = served;
        kos_sem_post(CH_DONE);
    }
    void frr_caller(void*) // caps: done@1, E(SIGNAL)@2
    {
        char buf[16];
        int32_t ok = 0;
        for (int i = 0; i < 2; i++)
        {
            memcpy(buf, "ping", 4);
            int32_t const rc = kos_call(2, buf, 4, sizeof(buf));
            if (rc == 5 and memcmp(buf, "pong!", 5) == 0)
            {
                ok++;
            }
        }
        g_frr_ok = ok;
        kos_sem_post(CH_DONE);
    }
    // KOS_CAP_NONE must skip reply resolution and allow a plain receive.
    Atomic<int32_t, Order::RELAXED> g_frr_heard{-99};
    void frr_recv_only(void*) // caps: done@1, E(WAIT)@2
    {
        char buf[16];
        struct kos_reply_recv_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.ep = 2;
        opts.timeout_us = KOS_TIMEOUT_NONE;
        int heard = 0;
        for (int i = 0; i < 2; i++)
        {
            opts.info.reply_cap = 0; // a zeroed one is stdout's reserved index, not the empty cap
            int32_t const n =
                kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
            if (n != 4 or memcmp(buf, "raw!", 4) != 0
                or opts.info.reply_cap != KOS_CAP_NONE)
            {
                break;
            }
            heard++;
        }
        g_frr_heard = heard;
        kos_sem_post(CH_DONE);
    }
    void frr_sender(void*) // caps: done@1, E(SIGNAL)@2
    {
        for (int i = 0; i < 2; i++)
        {
            (void)kos_send(2, "raw!", 4);
        }
        kos_sem_post(CH_DONE);
    }
    void t_reply_recv_no_reply()
    {
        kos_cap_grant rcaps[] = {{g_done, CH_FULL}, {0, EP_WAIT_ONLY}};
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {0, EP_SIGNAL_ONLY}};
        g_frr_heard = -99;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        rcaps[1].source_cap = g_ep;
        scaps[1].source_cap = g_ep;
        auto rv = kos::thread::create_caps(frr_recv_only, nullptr, "frrR", 10, rcaps, 2);
        kos::thread::Handle sd;
        if (rv.valid())
        {
            kos_sleep_ns(3000000ull); // let the receiver park in the fused receive
            sd = kos::thread::create_caps(frr_sender, nullptr, "frrD", 12, scaps, 2);
        }
        if (not rv.valid() or not sd.valid())
        {
            kos_handle_close(g_ep);
            tap::skip("pool too small for 2 threads");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(g_frr_heard.load() == 2);
    }

    // A receive error must still complete the reply's deferred wake. Pin the
    // higher-priority caller and server to one core so the caller must run
    // before the server continues.
    Atomic<int32_t, Order::RELAXED> g_frb_rc{-99};
    Atomic<uint32_t, Order::RELAXED> g_frb_caller_ran{0};  // written by the caller only
    Atomic<uint32_t, Order::RELAXED> g_frb_server_saw{9u}; // what the server read after its refusal
    void frb_server(void*)                                 // caps: done@1, E(WAIT)@2
    {
        char buf[16];
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 2, 0, KOS_TIMEOUT_NONE);
        int32_t const got =
            kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        if (got < 0 or opts.info.reply_cap == KOS_CAP_NONE)
        {
            g_frb_rc = -1;
            kos_sem_post(CH_DONE);
            return;
        }
        kos_cap_t const reply = opts.info.reply_cap;
        // An invalid endpoint fails the receive after the reply has woken its caller.
        kos_reply_recv_opts_init(&opts, 0x7fffu, 0, KOS_TIMEOUT_NONE);
        memcpy(buf, "pong!", 5);
        g_frb_rc = kos_reply_recv(reply, buf, kos_call_lens_pack(5, sizeof(buf)), &opts);
        g_frb_server_saw = g_frb_caller_ran.load();
        kos_sem_post(CH_DONE);
    }
    void frb_caller(void*) // caps: done@1, E(SIGNAL)@2
    {
        char buf[16];
        memcpy(buf, "ping", 4);
        (void)kos_call(2, buf, 4, sizeof(buf));
        g_frb_caller_ran = 1;
        kos_sem_post(CH_DONE);
    }
    void t_reply_recv_bad_ep_wakes_caller()
    {
        g_frb_rc = -99;
        g_frb_caller_ran = 0;
        g_frb_server_saw = 9u;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto sv = kos::thread::create_caps(frb_server, nullptr, "frbS", TAP_PRIO_PARKS, scaps,
                                           2, KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                           nullptr, KOS_TASK_NONE, nullptr, 0, TAP_PIN_CORE);
        kos::thread::Handle cl;
        if (sv.valid())
        {
            kos_sleep_ns(3000000ull); // let the server park in its receive
            // A higher-priority caller on the same core must preempt the server.
            cl = kos::thread::create_caps(frb_caller, nullptr, "frbC", 20, ccaps, 2,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                          KOS_TASK_NONE, nullptr, 0, TAP_PIN_CORE);
        }
        if (not sv.valid() or not cl.valid())
        {
            kos_handle_close(g_ep);
            tap::skip("pool too small for 2 threads");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        tap::diag("fused refusal %d, caller had run: %u", static_cast<int>(g_frb_rc.load()),
                  static_cast<unsigned>(g_frb_server_saw.load()));
        TAP_CHECK(g_frb_rc.load() == -KOS_EBADF);
        TAP_CHECK(g_frb_server_saw.load() == 1u);
    }

    // A client reply-buffer fault must not terminate a shared service.
    // Use buffers with an offset: the short request does not overlap, but the
    // longer reply does and ep_copy rejects it. Then serve a second client.
    constexpr size_t F4_SKEW = 8;       // server buffer start inside the client's
    constexpr size_t F4_REQ_LEN = 4;    // <= F4_SKEW, so the request does not overlap
    constexpr size_t F4_REPLY_LEN = 24; // > F4_SKEW, so the reply does
    constexpr size_t F4_CAP = 32;
    constexpr int F4_CALLS = 2;
    // Bound calls with a deadline. main retains a WAIT cap, so a service exit
    // would not trigger the last-receiver wake.
    constexpr uint32_t F4_CALL_US = STALL_TOLERANT_US;
    alignas(8) uint8_t g_f4_aliased[F4_SKEW + F4_CAP]; // overlapping client/server buffers
    alignas(8) uint8_t g_f4_clean[F4_CAP]; // separate client buffer
    Atomic<int32_t, Order::RELAXED> g_f4_bad{-99};
    Atomic<int32_t, Order::RELAXED> g_f4_good{-99};
    Atomic<int32_t, Order::RELAXED> g_f4_served{-99};
    Atomic<int32_t, Order::RELAXED> g_f4_dropped{-99};

    void f4_service(void*) // caps: done@1, E(WAIT)@2
    {
        struct kos_reply_recv_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.ep = 2;
        opts.timeout_us = STALL_TOLERANT_US;
        kos_cap_t reply_cap = KOS_CAP_NONE;
        size_t reply_len = 0;
        int served = 0;
        int dropped = 0;
        while (served < F4_CALLS)
        {
            opts.info.reply_cap = KOS_CAP_NONE;
            int32_t const n = kos_reply_recv(reply_cap, g_f4_aliased + F4_SKEW,
                                             kos_call_lens_pack(reply_len, F4_CAP), &opts);
            reply_cap = KOS_CAP_NONE;
            reply_len = 0;
            if (n < 0)
            {
                // Exercise the same error classifier as production service loops.
                if (kickos::serve_transaction_failed(n))
                {
                    dropped++;
                    continue;
                }
                break;
            }
            if (opts.info.reply_cap == KOS_CAP_NONE)
            {
                continue;
            }
            served++;
            memcpy(g_f4_aliased + F4_SKEW, "pong!", 5);
            reply_len = F4_REPLY_LEN;
            reply_cap = opts.info.reply_cap;
        }
        if (reply_cap != KOS_CAP_NONE)
        {
            (void)kos_reply(reply_cap, g_f4_aliased + F4_SKEW, reply_len);
        }
        g_f4_served = served;
        g_f4_dropped = dropped;
        kos_sem_post(CH_DONE);
    }
    void f4_client(void*) // caps: done@1, E(SIGNAL)@2
    {
        memcpy(g_f4_aliased, "ping", F4_REQ_LEN);
        g_f4_bad = kos_call_timed(2, g_f4_aliased, F4_REQ_LEN, F4_CAP, F4_CALL_US);
        memcpy(g_f4_clean, "ping", F4_REQ_LEN);
        g_f4_good = kos_call_timed(2, g_f4_clean, F4_REQ_LEN, F4_CAP, F4_CALL_US);
        kos_sem_post(CH_DONE);
    }
    void t_service_survives_client_fault()
    {
        g_f4_bad = -99;
        g_f4_good = -99;
        g_f4_served = -99;
        g_f4_dropped = -99;
        memset(g_f4_aliased, 0, sizeof(g_f4_aliased));
        memset(g_f4_clean, 0, sizeof(g_f4_clean));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto sv = kos::thread::create_caps(f4_service, nullptr, "f4S", 10, scaps, 2);
        kos::thread::Handle cl;
        if (sv.valid())
        {
            kos_sleep_ns(3000000ull); // let the service park in its receive
            cl = kos::thread::create_caps(f4_client, nullptr, "f4C", 12, ccaps, 2);
        }
        if (not sv.valid() or not cl.valid())
        {
            // Slain rather than awaited, so its done post cannot land in a later arm's count.
            if (sv.valid())
            {
                (void)sv.slay(STALL_TOLERANT_US);
            }
            kos_handle_close(g_ep);
            tap::skip("pool too small for 2 threads");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        tap::diag("aliased reply %d, next call %d, served %d, dropped %d",
                  static_cast<int>(g_f4_bad.load()), static_cast<int>(g_f4_good.load()),
                  static_cast<int>(g_f4_served.load()),
                  static_cast<int>(g_f4_dropped.load()));
        TAP_CHECK(g_f4_bad.load() == -KOS_EFAULT);
        TAP_CHECK(g_f4_dropped.load() == 1);
        TAP_CHECK(g_f4_good.load() == static_cast<int32_t>(F4_REPLY_LEN));
        TAP_CHECK(g_f4_served.load() == F4_CALLS);
    }

    // No sender or accepted IRQ: only the receive deadline can end this wait.
    Atomic<int32_t, Order::RELAXED> g_frt_rc{-99};
    void frr_timeout_server(void*) // caps: done@1, E(WAIT)@2
    {
        char buf[16];
        struct kos_reply_recv_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.ep = 2;
        opts.timeout_us = 20000u; // 20 ms
        g_frt_rc = kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        kos_sem_post(CH_DONE);
    }
    void t_reply_recv_timeout()
    {
        g_frt_rc = -99;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        auto sv = kos::thread::create_caps(frr_timeout_server, nullptr, "frrT", 10, caps, 2);
        if (not sv.valid())
        {
            kos_handle_close(g_ep);
            tap::skip("pool too small for the timeout server");
            return;
        }
        wait_n(1);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(g_frt_rc.load() == -KOS_ETIMEDOUT);
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // Raise two bound IRQs, consume both bits, then repeat to verify rearming.
    // Use a deadline so a failure reports instead of hanging. These lines are
    // not held by other tests while this test runs.
    constexpr uint32_t FRN_WAIT_US = 200u * 1000u;
    constexpr int FRN_LINE2 = KICKOS_IRQ_FREE_BASE + 3;
    constexpr int FRN_CH_EP = 2;
    constexpr int FRN_CH_NOTE = 3;
    constexpr int FRN_CH_GO = 4;
    // The two lines raise DIFFERENT bits of one object, because main attached each through a
    // copy badged for that bit. The server takes the unbadged capability and so waits on
    // both. Their distinctness is witnessed by round 2 below, which accepts one and leaves
    // the other pending, and not by an attach call that could hand back the same bit twice.
    constexpr uint32_t FRN_BIT1 = 1u << 0;
    constexpr uint32_t FRN_BIT2 = 1u << 1;
    kos_cap_t g_frn_go = KOS_CAP_NONE;
    Atomic<int32_t, Order::RELAXED> g_frn_r1{-99};
    Atomic<int32_t, Order::RELAXED> g_frn_r2{-99};
    Atomic<uint32_t, Order::RELAXED> g_frn_bits1{0};
    Atomic<uint32_t, Order::RELAXED> g_frn_bits2{0};
    Atomic<int32_t, Order::RELAXED> g_frn_r3{-99};
    Atomic<int32_t, Order::RELAXED> g_frn_r4{-99};
    Atomic<uint32_t, Order::RELAXED> g_frn_bits3{0};
    Atomic<uint32_t, Order::RELAXED> g_frn_bits4{0};
    Atomic<uint32_t, Order::RELAXED> g_frn_mask{0};
    Atomic<uint32_t, Order::RELAXED> g_frn_m1{0};
    Atomic<uint32_t, Order::RELAXED> g_frn_m2{0};
    void frr_notify_server(void*) // caps: done@1, E(WAIT)@2, note@3, go@4
    {
        char buf[16];
        if (kos_notify_bind(FRN_CH_NOTE) != 0)
        {
            kos_sem_post(CH_DONE);
            return;
        }
        uint32_t const m1 = FRN_BIT1;
        uint32_t const m2 = FRN_BIT2;
        g_frn_mask = m1 | m2;
        g_frn_m1 = m1;
        g_frn_m2 = m2;
        struct kos_reply_recv_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.ep = FRN_CH_EP;
        for (int round = 0; round < 3; round++)
        {
            // Wait on a semaphore while main raises both IRQs. Semaphore waits
            // leave notification bits pending.
            kos_sem_wait(FRN_CH_GO);
            // Round 2 accepts only one line.
            uint32_t accept = m1 | m2;
            if (round == 2)
            {
                accept = m1;
            }
            opts.notify = accept; // IN: the lines this wait accepts
            opts.timeout_us = FRN_WAIT_US;
            int32_t const n =
                kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
            if (round == 0)
            {
                g_frn_bits1 = opts.notify; // OUT: the bits it consumed
                g_frn_r1 = n;
            }
            else if (round == 1)
            {
                g_frn_bits2 = opts.notify;
                g_frn_r2 = n;
            }
            else
            {
                g_frn_bits3 = opts.notify;
                g_frn_r3 = n;
                // The unaccepted line must remain pending for the next wait.
                opts.notify = m2;
                opts.timeout_us = FRN_WAIT_US;
                g_frn_r4 = kos_reply_recv(KOS_CAP_NONE, buf,
                                          kos_call_lens_pack(0, sizeof(buf)), &opts);
                g_frn_bits4 = opts.notify;
            }
            // No ack: the next receive's own accept mask is what rearms the lines chained on
            // this object, and round 2 leaving one unaccepted is what says it rearms only
            // those.
        }
        kos_sem_post(CH_DONE);
    }
    // Claim, attach, bind and arm without raising the line; only the timeout can end the
    // wait.
    Atomic<int32_t, Order::RELAXED> g_irqto_rc{-99};
    void irqto_worker(void*) // caps: done@1, note(FULL)@2
    {
        auto note = kos::Notification::adopt(2);
        if (note.bind() != 0)
        {
            kos_sem_post(CH_DONE);
            return;
        }
        uint32_t bits = 0;
        g_irqto_rc = note.wait(NOTE_ALL, 20000u, &bits); // 20 ms
        kos_sem_post(CH_DONE);
    }
    void t_irq_wait_timeout()
    {
        g_irqto_rc = -99;
        kos_cap_t line = KOS_CAP_NONE;
        TAP_CHECK(irq_claim_await(IRQ_CTX_LINE, &line) == 0);
        kos_cap_t note = notify_for_line(line);
        TAP_CHECK(note != KOS_CAP_NONE);
        kos_irq_ack(line); // armed, and still nothing raises it
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {note, CH_FULL}};
        auto w = irq_spawn(irqto_worker, nullptr, "irqT", 10, caps, 2);
        if (not w.valid())
        {
            kos_handle_close(line);
            kos_handle_close(note);
            tap::skip("pool too small for the timed-wait worker");
            return;
        }
        wait_n(1);
        kos_handle_close(line);
        kos_handle_close(note);
        TAP_CHECK(g_irqto_rc.load() == -KOS_ETIMEDOUT);
    }

    // Test notification delivery to a parked receive, including the resume barrier.
    // The lower-priority raiser runs only after the server blocks. A deferred-switch
    // backend must complete that switch before reading notification bits.
    Atomic<int32_t, Order::RELAXED> g_frp_rc{-99};
    Atomic<uint32_t, Order::RELAXED> g_frp_bits{0};
    Atomic<uint32_t, Order::RELAXED> g_frp_mask{0};
    void frp_server(void*) // caps: done@1, E(WAIT)@2, note@3
    {
        char buf[16];
        if (kos_notify_bind(3) != 0)
        {
            kos_sem_post(CH_DONE);
            return;
        }
        uint32_t const mask = FRN_BIT1; // main attached the line through an unbadged copy
        g_frp_mask = mask;
        struct kos_reply_recv_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.ep = 2;
        opts.notify = mask; // no event pending; this receive must park
        opts.timeout_us = FRN_WAIT_US;
        g_frp_rc =
            kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        g_frp_bits = opts.notify;
        kos_sem_post(CH_DONE);
    }
    void frp_raiser(void*) // caps: done@1
    {
        kos_irq_inject(IRQ_CTX_LINE);
        kos_sem_post(CH_DONE);
    }
    void t_reply_recv_notify_park()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        kos_cap_t line = KOS_CAP_NONE;
        TAP_CHECK(irq_claim_await(IRQ_CTX_LINE, &line) == 0);
        kos_cap_t note = notify_for_line(line); // unbadged: this line raises bit 0
        TAP_CHECK(note != KOS_CAP_NONE);
        kos_irq_ack(line); // a claim leaves the line masked
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        g_frp_rc = -99;
        g_frp_bits = 0;
        g_frp_mask = 0;
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}, {note, CH_FULL}};
        kos_cap_grant rcaps[] = {{g_done, CH_FULL}};
        auto sv = irq_spawn(frp_server, nullptr, "frpS", 12, scaps, 3);
        kos::thread::Handle rz;
        if (sv.valid())
        {
            rz = kos::thread::create_caps(frp_raiser, nullptr, "frpR", 8, rcaps, 1);
        }
        if (not sv.valid() or not rz.valid())
        {
            kos_handle_close(g_ep);
            kos_handle_close(line);
            kos_handle_close(note);
            tap::skip("pool too small for the parked notify arm");
            return;
        }
        wait_n(2);
        kos_handle_close(g_ep);
        kos_handle_close(line);
        kos_handle_close(note);
        uint32_t const mask = g_frp_mask.load();
        TAP_CHECK(mask != 0);
        TAP_CHECK(g_frp_rc.load() == -KOS_ENOTIFY and g_frp_bits.load() == mask);
    }

    // Whether a batch's first refusal is a supply running out, the only refusal an arm whose
    // demand is OPTIONAL may answer with a skip; any other code is the arm failing.
    bool refused_for_want(int rc)
    {
        return rc == -KOS_EMFILE or rc == -KOS_EAGAIN or rc == -KOS_ENOMEM;
    }
    void t_reply_recv_notify()
    {
        // This ordering requires the server to reach its gate before main injects.
        TAP_SKIP_ONE_CORE_ORDER();
        // Five capabilities live at once, two at the badge mints and the endpoint plus the
        // gate afterwards: more than the suite's mandatory per-arm peak, so this arm draws on
        // the OPTIONAL demand and reclaims and skips where the board did not grant it. A
        // refusal reached as a TAP_CHECK would leak the lines and the object into every
        // later arm.
        kos_cap_t l1 = KOS_CAP_NONE;
        kos_cap_t l2 = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        kos_cap_t b1 = KOS_CAP_NONE;
        kos_cap_t b2 = KOS_CAP_NONE;
        g_ep = KOS_CAP_NONE;
        g_frn_go = KOS_CAP_NONE;
        int refused = 0;
        note_refusal(irq_claim_await(IRQ_CTX_LINE, &l1), &refused);
        note_refusal(irq_claim_await(FRN_LINE2, &l2), &refused);
        // One object, two lines, one bit each: the badge is what tells them apart, and it is
        // seated at the MINT, so main holds two badged copies just long enough to attach.
        note_refusal(kos_notify_create(&note), &refused);
        if (note != KOS_CAP_NONE)
        {
            note_refusal(kos_notify_badge(note, 0u, &b1), &refused);
            note_refusal(kos_notify_badge(note, 1u, &b2), &refused);
        }
        if (refused == 0)
        {
            TAP_CHECK(kos_irq_bind_notify(l1, b1) == 0);
            TAP_CHECK(kos_irq_bind_notify(l2, b2) == 0);
        }
        // The attachment holds its own reference.
        if (b1 != KOS_CAP_NONE) { TAP_CHECK(kos_handle_close(b1) == 0); }
        if (b2 != KOS_CAP_NONE) { TAP_CHECK(kos_handle_close(b2) == 0); }
        if (refused == 0)
        {
            note_refusal(kos_endpoint_create(&g_ep), &refused);
            note_refusal(kos_sem_create(0, &g_frn_go), &refused);
        }
        if (refused != 0)
        {
            if (g_ep != KOS_CAP_NONE) { kos_handle_close(g_ep); g_ep = KOS_CAP_NONE; }
            if (g_frn_go != KOS_CAP_NONE) { kos_sem_destroy(g_frn_go); g_frn_go = KOS_CAP_NONE; }
            if (l1 != KOS_CAP_NONE) { kos_handle_close(l1); }
            if (l2 != KOS_CAP_NONE) { kos_handle_close(l2); }
            if (note != KOS_CAP_NONE) { kos_handle_close(note); }
            TAP_CHECK(refused_for_want(refused));
            // Labelled by the supply that ran out, as t_mutex_deadlock does.
            char const* why = "pool too small";
            if (refused == -KOS_EMFILE)
            {
                why = "cap table too small (5 concurrent caps)";
            }
            if (refused == -KOS_EAGAIN)
            {
                why = "task object budget too small (5 concurrent objects)";
            }
            tap::skip("%s", why);
            return;
        }
        // Arm before the first injection.
        kos_irq_ack(l1);
        kos_irq_ack(l2);
        g_frn_r1 = -99;
        g_frn_r2 = -99;
        g_frn_r3 = -99;
        g_frn_r4 = -99;
        g_frn_bits1 = 0;
        g_frn_bits2 = 0;
        g_frn_bits3 = 0;
        g_frn_bits4 = 0;
        g_frn_mask = 0;
        kos_cap_grant caps[] = {{g_done, CH_FULL},
                                {g_ep, EP_WAIT_ONLY},
                                {note, CH_FULL},
                                {g_frn_go, CH_FULL}};
        auto sv = irq_spawn(frr_notify_server, nullptr, "frrN", 10, caps, 4);
        if (not sv.valid())
        {
            kos_handle_close(g_ep);
            kos_sem_destroy(g_frn_go);
            kos_handle_close(l1);
            kos_handle_close(l2);
            kos_handle_close(note);
            tap::skip("pool too small for the notify server");
            return;
        }
        for (int round = 0; round < 3; round++)
        {
            kos_sleep_ns(3000000ull); // let the server bind and park on the gate
            // main rearms, because main is the one holding the line capabilities: the server
            // holds only the object. Both raises must be pending before the wait looks, or
            // the wait parks on the first and the arm measures which ISR won rather than
            // which bits the mask accepted.
            kos_irq_ack(l1);
            kos_irq_ack(l2);
            kos_irq_inject(IRQ_CTX_LINE);
            kos_irq_inject(FRN_LINE2);
            kos_sem_post(g_frn_go);
        }
        wait_n(1);
        kos_handle_close(g_ep);
        kos_sem_destroy(g_frn_go);
        kos_handle_close(l1);
        kos_handle_close(l2);
        kos_handle_close(note);
        uint32_t const mask = g_frn_mask.load();
        TAP_CHECK(mask != 0);
        // One wait consumes both accepted bits.
        TAP_CHECK(g_frn_r1.load() == -KOS_ENOTIFY and g_frn_bits1.load() == mask);
        // Repeat to check rearming.
        TAP_CHECK(g_frn_r2.load() == -KOS_ENOTIFY and g_frn_bits2.load() == mask);
        // Accept one line and leave the other pending for the next wait.
        TAP_CHECK(g_frn_r3.load() == -KOS_ENOTIFY and g_frn_bits3.load() == g_frn_m1.load());
        TAP_CHECK(g_frn_r4.load() == -KOS_ENOTIFY and g_frn_bits4.load() == g_frn_m2.load());
    }
#endif // KICKOS_ENABLE_SELFTEST (fused notification)

    void t_reply_recv_loop()
    {
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {0, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {0, EP_SIGNAL_ONLY}};
        g_frr_served = -99;
        g_frr_ok = -99;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        scaps[1].source_cap = g_ep;
        ccaps[1].source_cap = g_ep;
        auto sv = kos::thread::create_caps(frr_server, nullptr, "frrS", 10, scaps, 2);
        kos::thread::Handle cl;
        if (sv.valid())
        {
            kos_sleep_ns(3000000ull); // let the server park in the fused receive
            cl = kos::thread::create_caps(frr_caller, nullptr, "frrC", 12, ccaps, 2);
        }
        if (not sv.valid() or not cl.valid())
        {
            kos_handle_close(g_ep);
            tap::skip("pool too small for 2 threads");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(g_frr_served.load() == 2);
        TAP_CHECK(g_frr_ok.load() == 2);
    }

    void t_call_happy()
    {
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {0, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {0, EP_SIGNAL_ONLY}};

        // (A) fastpath: server parks in recv first, then the caller calls.
        g_echo_reqn = -99; g_echo_rc = -99;
        memset(g_echo_reqbuf, 0, sizeof(g_echo_reqbuf));
        memset(g_echo_rplbuf, 0, sizeof(g_echo_rplbuf));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        scaps[1].source_cap = g_ep;
        ccaps[1].source_cap = g_ep;
        auto sv = kos::thread::create_caps(echo_server, nullptr, "echS", 10, scaps, 2);
        kos::thread::Handle cl;
        if (sv.valid())
        {
            kos_sleep_ns(3000000ull); // let the server park in recv (fastpath)
            cl = kos::thread::create_caps(echo_caller, nullptr, "echC", 12, ccaps, 2);
        }
        if (not sv.valid() or not cl.valid())
        {
            // cl is spawned only after sv succeeds, so a skip means nothing spawned or a lone
            // server parked in recv. Neither can be drained: close and skip.
            kos_handle_close(g_ep);
            tap::skip("pool too small for 2 threads");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        int32_t const echo_reqn_a = g_echo_reqn;
        int32_t const echo_rc_a = g_echo_rc;
        TAP_CHECK(echo_reqn_a == 4 and memcmp(g_echo_reqbuf, "ping", 4) == 0);
        TAP_CHECK(echo_rc_a == 5 and memcmp(g_echo_rplbuf, "pong!", 5) == 0);

        // (B) slowpath: caller parks in SEND_WAIT first, server recvs later.
        g_echo_reqn = -99; g_echo_rc = -99;
        memset(g_echo_reqbuf, 0, sizeof(g_echo_reqbuf));
        memset(g_echo_rplbuf, 0, sizeof(g_echo_rplbuf));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        scaps[1].source_cap = g_ep;
        ccaps[1].source_cap = g_ep;
        auto cl2 = kos::thread::create_caps(echo_caller, nullptr, "echC2", 12, ccaps, 2);
        kos::thread::Handle sv2;
        if (cl2.valid())
        {
            kos_sleep_ns(3000000ull); // let the caller park in SEND_WAIT (slowpath)
            sv2 = kos::thread::create_caps(echo_server, nullptr, "echS2", 10, scaps, 2);
        }
        if (not cl2.valid() or not sv2.valid())
        {
            // The caller (spawned first) may be parked in SEND_WAIT: close FIRST so it is
            // EPIPE'd and posts, THEN drain it.
            kos_handle_close(g_ep);
            if (cl2.valid()) { wait_n(1); }
            tap::partial("slowpath half not run (pool too small)");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        int32_t const echo_reqn_b = g_echo_reqn;
        int32_t const echo_rc_b = g_echo_rc;
        TAP_CHECK(echo_reqn_b == 4 and memcmp(g_echo_reqbuf, "ping", 4) == 0);
        TAP_CHECK(echo_rc_b == 5 and memcmp(g_echo_rplbuf, "pong!", 5) == 0);
    }

#if defined(KICKOS_ENABLE_SELFTEST) // kos_ipc_fast_taken is a selftest-only syscall
    // --- Call/reply: the trap-handler register fastpath ------------------------------
    // The fastpath and the buffer form answer a caller IDENTICALLY, so kos_ipc_fast_taken is
    // the only witness that the arm under test ran at all, and every assertion below pairs a
    // content check with a counter delta. The peers MUST run at equal priority: the fastpath
    // refuses when the caller outranks the server. The reply travels back through the
    // caller's saved trap frame, so the server transforms the request: a reply byte-identical
    // to the request cannot distinguish a correct copy from a buffer the kernel left alone.
    constexpr unsigned char FP_XOR = 0xA5;
    constexpr size_t FP_MAX = 20; // KOS_CALL_REG_BYTES
    // The content checks hold either way, so the arm also covers the buffer form where the
    // backend has no fastpath and the counter cannot move.
#if KICKOS_ARCH_HAS_IPC_FASTPATH
    constexpr uint32_t FP_EXPECT_TAKEN = 1;
#else
    constexpr uint32_t FP_EXPECT_TAKEN = 0;
#endif
    Atomic<int32_t, Order::RELAXED> g_fp_reqn{-99};
    Atomic<int32_t, Order::RELAXED> g_fp_rc{-99};
    unsigned char g_fp_rpl[FP_MAX];
    size_t g_fp_send_len = 0;
    size_t g_fp_recv_cap = 0;

    void fp_server(void*) // caps: done@1, E(WAIT)@2
    {
        unsigned char buf[64];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 2, 0, KOS_TIMEOUT_NONE);
        int32_t const n = kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        info = opts.info;
        g_fp_reqn = n;
        if (info.reply_cap != KOS_CAP_NONE and n >= 0)
        {
            for (int32_t i = 0; i < n; i++)
            {
                buf[i] = static_cast<unsigned char>(buf[i] ^ FP_XOR);
            }
            kos_reply(info.reply_cap, buf, static_cast<size_t>(n));
        }
        kos_sem_post(CH_DONE);
    }
    void fp_caller(void*) // caps: done@1, E(SIGNAL)@2
    {
        unsigned char buf[64];
        for (size_t i = 0; i < sizeof(buf); i++)
        {
            buf[i] = static_cast<unsigned char>(i + 1); // no zero byte: a cleared buffer shows
        }
        g_fp_rc = kos_call(2, buf, g_fp_send_len, g_fp_recv_cap);
        int32_t const rc = g_fp_rc;
        if (rc > 0)
        {
            size_t k = static_cast<size_t>(rc);
            if (k > FP_MAX)
            {
                k = FP_MAX;
            }
            memcpy(g_fp_rpl, buf, k);
        }
        kos_sem_post(CH_DONE);
    }
    // Runs one call at the given lengths and reports how far the fastpath counter moved.
    // Takes no TAP_CHECK: that returns from its enclosing function, which here would
    // abandon the peers mid-transaction and strand the caller's semaphore posts.
    enum FpStatus
    {
        FP_RAN = 0,
        FP_NO_POOL = 1,  // the pool could not seat both peers
        FP_EP_REFUSED = 2 // endpoint create or close refused
    };
    int fp_run(size_t send_len, size_t recv_cap, uint32_t* taken_delta)
    {
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {0, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {0, EP_SIGNAL_ONLY}};
        g_fp_send_len = send_len;
        g_fp_recv_cap = recv_cap;
        g_fp_reqn = -99;
        g_fp_rc = -99;
        memset(g_fp_rpl, 0, sizeof(g_fp_rpl));
        if (kos_endpoint_create(&g_ep) != 0)
        {
            return FP_EP_REFUSED;
        }
        scaps[1].source_cap = g_ep;
        ccaps[1].source_cap = g_ep;
        uint32_t const before = kos_ipc_fast_taken();
        auto sv = kos::thread::create_caps(fp_server, nullptr, "fpS", 11, scaps, 2);
        kos::thread::Handle cl;
        if (sv.valid())
        {
            kos_sleep_ns(3000000ull); // the server must be PARKED in recv before the call
            cl = kos::thread::create_caps(fp_caller, nullptr, "fpC", 11, ccaps, 2);
        }
        if (not sv.valid() or not cl.valid())
        {
            kos_handle_close(g_ep);
            return FP_NO_POOL;
        }
        wait_n(2);
        *taken_delta = kos_ipc_fast_taken() - before;
        if (kos_handle_close(g_ep) != 0)
        {
            return FP_EP_REFUSED;
        }
        return FP_RAN;
    }
    void t_call_reg_fastpath()
    {
        uint32_t taken = 0;

        // (A) both lengths inside the register budget: the fastpath runs.
        int st = fp_run(8, 8, &taken);
        if (st == FP_NO_POOL)
        {
            tap::skip("pool too small for 2 threads");
            return;
        }
        TAP_CHECK(st == FP_RAN);
        int32_t const rc_a = g_fp_rc;
        TAP_CHECK(g_fp_reqn == 8);
        TAP_CHECK(rc_a == 8);
        bool ok_a = true;
        for (size_t i = 0; i < 8; i++)
        {
            if (g_fp_rpl[i] != static_cast<unsigned char>((i + 1) ^ FP_XOR))
            {
                ok_a = false;
            }
        }
        TAP_CHECK(ok_a); // every reply byte, through the caller's saved trap frame
        TAP_CHECK(taken == FP_EXPECT_TAKEN);

        // (B) the boundary, exactly KOS_CALL_REG_BYTES each way.
        st = fp_run(FP_MAX, FP_MAX, &taken);
        if (st == FP_NO_POOL)
        {
            tap::partial("boundary half not run (pool too small)");
            return;
        }
        TAP_CHECK(st == FP_RAN);
        int32_t const rc_b = g_fp_rc;
        TAP_CHECK(g_fp_reqn == static_cast<int32_t>(FP_MAX));
        TAP_CHECK(rc_b == static_cast<int32_t>(FP_MAX));
        bool ok_b = true;
        for (size_t i = 0; i < FP_MAX; i++)
        {
            if (g_fp_rpl[i] != static_cast<unsigned char>((i + 1) ^ FP_XOR))
            {
                ok_b = false;
            }
        }
        TAP_CHECK(ok_b);
        TAP_CHECK(taken == FP_EXPECT_TAKEN);

        // (C) a reply capacity ABOVE the budget keeps the buffer form, and the counter is
        // what says so: the bytes alone would look the same either way.
        st = fp_run(8, 64, &taken);
        if (st == FP_NO_POOL)
        {
            tap::partial("buffer-form half not run (pool too small)");
            return;
        }
        TAP_CHECK(st == FP_RAN);
        int32_t const rc_c = g_fp_rc;
        TAP_CHECK(g_fp_reqn == 8);
        TAP_CHECK(rc_c == 8);
        bool ok_c = true;
        for (size_t i = 0; i < 8; i++)
        {
            if (g_fp_rpl[i] != static_cast<unsigned char>((i + 1) ^ FP_XOR))
            {
                ok_c = false;
            }
        }
        TAP_CHECK(ok_c);
        TAP_CHECK(taken == 0);
    }
#endif // KICKOS_ENABLE_SELFTEST (register-fastpath witness)

    // --- Call/reply: main calls like any other thread --------------------------------
    // main holds an ordinary thread-pool slot, so a reply capability can name it. Both
    // dispatch sites run against one spawned echo server: (A) the server parked in recv
    // before main calls, (B) main parked in SEND_WAIT first. The server MUST exist before
    // the call either way, or the call is -KOS_EPIPE, and it runs at KICKOS_PRIO_MIN, one
    // below main: above main it could park in recv between the spawn and the call, and every
    // donation site is `>`-guarded, so only a server main outranks makes main DONATE.
    void t_call_from_root()
    {
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {0, EP_WAIT_ONLY}};
        char buf[16];

        g_echo_reqn = -99;
        memset(g_echo_reqbuf, 0, sizeof(g_echo_reqbuf));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        scaps[1].source_cap = g_ep;
        auto sv = kos::thread::create_caps(echo_server, nullptr, "rtS", 1, scaps, 2);
        if (not sv.valid())
        {
            kos_handle_close(g_ep);
            tap::skip("pool too small for 1 thread");
            return;
        }
        kos_sleep_ns(3000000ull); // main yields: the server runs and parks in recv (fastpath)
        memcpy(buf, "ping", 4);
        int32_t rc = kos_call(g_ep, buf, 4, sizeof(buf)); // reply lands back in buf
        wait_n(1);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        int32_t const echo_reqn_a = g_echo_reqn;
        TAP_CHECK(echo_reqn_a == 4 and memcmp(g_echo_reqbuf, "ping", 4) == 0);
        TAP_CHECK(rc == 5 and memcmp(buf, "pong!", 5) == 0);
        kos_sleep_ns(3000000ull); // the server reaches EXITED, so its slot is reclaimable

        g_echo_reqn = -99;
        memset(g_echo_reqbuf, 0, sizeof(g_echo_reqbuf));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        scaps[1].source_cap = g_ep;
        auto sv2 = kos::thread::create_caps(echo_server, nullptr, "rtS2", 1, scaps, 2);
        if (not sv2.valid())
        {
            kos_handle_close(g_ep);
            tap::partial("slowpath half not run (pool too small)");
            return;
        }
        memcpy(buf, "ping", 4);
        rc = kos_call(g_ep, buf, 4, sizeof(buf)); // no receiver parked yet: main blocks first
        wait_n(1);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        int32_t const echo_reqn_b = g_echo_reqn;
        TAP_CHECK(echo_reqn_b == 4 and memcmp(g_echo_reqbuf, "ping", 4) == 0);
        TAP_CHECK(rc == 5 and memcmp(buf, "pong!", 5) == 0);
        kos_sleep_ns(3000000ull); // the server reaches EXITED, so its slot is reclaimable
    }

    // --- Call/reply: reply + request truncation (datagram clamp, not an error) ---
    // One call exercises BOTH clamps: the caller sends 8 bytes into a server recv buffer of
    // 3 (request truncated to 3), and the server replies 8 bytes into a caller recv_cap of 3
    // (reply truncated to 3). Neither is an error: the byte counts just clamp.
    Atomic<int32_t, Order::RELAXED> g_trunc_reqn{-99}; // request bytes the server saw (its buffer < send_len)
    char g_trunc_reqbuf[4];
    Atomic<int32_t, Order::RELAXED> g_trunc_rc{-99}; // caller's kos_call return (clamped to recv_cap)
    char g_trunc_rplbuf[4];
    void trunc_server(void*) // caps: done@1, E(WAIT)@2
    {
        char buf[3]; // smaller than the 8-byte request
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 2, 0, KOS_TIMEOUT_NONE);
        int32_t n = kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        info = opts.info;
        g_trunc_reqn = n;
        if (n > 0)
        {
            size_t k = static_cast<size_t>(n);
            if (k > sizeof(g_trunc_reqbuf)) { k = sizeof(g_trunc_reqbuf); }
            memcpy(g_trunc_reqbuf, buf, k);
        }
        if (info.reply_cap != KOS_CAP_NONE)
        {
            char rpl[8];
            memcpy(rpl, "12345678", 8);
            kos_reply(info.reply_cap, rpl, 8); // 8 offered, caller cap is 3 -> clamps to 3
        }
        kos_sem_post(CH_DONE);
    }
    void trunc_caller(void*) // caps: done@1, E(SIGNAL)@2
    {
        char buf[8];
        memcpy(buf, "ABCDEFGH", 8);
        g_trunc_rc = kos_call(2, buf, 8, 3); // recv_cap = 3 -> the reply clamps into it
        if (g_trunc_rc > 0)
        {
            size_t k = static_cast<size_t>(g_trunc_rc);
            if (k > sizeof(g_trunc_rplbuf)) { k = sizeof(g_trunc_rplbuf); }
            memcpy(g_trunc_rplbuf, buf, k);
        }
        kos_sem_post(CH_DONE);
    }
    void t_call_truncation()
    {
        g_trunc_reqn = -99; g_trunc_rc = -99;
        memset(g_trunc_reqbuf, 0, sizeof(g_trunc_reqbuf));
        memset(g_trunc_rplbuf, 0, sizeof(g_trunc_rplbuf));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto sv = kos::thread::create_caps(trunc_server, nullptr, "trS", 10, scaps, 2);
        kos::thread::Handle cl;
        if (sv.valid())
        {
            kos_sleep_ns(3000000ull);
            cl = kos::thread::create_caps(trunc_caller, nullptr, "trC", 12, ccaps, 2);
        }
        if (not sv.valid() or not cl.valid())
        {
            kos_handle_close(g_ep); // lone parked server or nothing spawned: nothing to drain
            tap::skip("pool too small");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        int32_t const trunc_reqn = g_trunc_reqn;
        int32_t const trunc_rc = g_trunc_rc;
        TAP_CHECK(trunc_reqn == 3 and memcmp(g_trunc_reqbuf, "ABC", 3) == 0);
        TAP_CHECK(trunc_rc == 3 and memcmp(g_trunc_rplbuf, "123", 3) == 0);
    }

    // --- Call/reply: a second reply on a consumed cap is rejected -----------------
    // The reply cap is one-shot: the first kos_reply consumes it (empty slot + gen bump), so
    // a second kos_reply on the same handle fails resolve with -KOS_EBADF.
    Atomic<int, Order::RELAXED> g_dr_second{-99}; // second kos_reply rc
    Atomic<int32_t, Order::RELAXED> g_dr_callrc{-99};
    void dr_server(void*) // caps: done@1, E(WAIT)@2
    {
        char buf[16];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 2, 0, KOS_TIMEOUT_NONE);
        kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        info = opts.info;
        if (info.reply_cap != KOS_CAP_NONE)
        {
            char rpl[4];
            memcpy(rpl, "ok", 2);
            kos_reply(info.reply_cap, rpl, 2);                  // consumes the cap
            g_dr_second = kos_reply(info.reply_cap, rpl, 2);    // cap gone -> -KOS_EBADF
        }
        kos_sem_post(CH_DONE);
    }
    void dr_caller(void*) // caps: done@1, E(SIGNAL)@2
    {
        char buf[8] = {0};
        g_dr_callrc = kos_call(2, buf, 4, sizeof(buf));
        kos_sem_post(CH_DONE);
    }
    void t_call_double_reply()
    {
        g_dr_second = -99; g_dr_callrc = -99;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto sv = kos::thread::create_caps(dr_server, nullptr, "drS", 10, scaps, 2);
        kos::thread::Handle cl;
        if (sv.valid())
        {
            kos_sleep_ns(3000000ull);
            cl = kos::thread::create_caps(dr_caller, nullptr, "drC", 12, ccaps, 2);
        }
        if (not sv.valid() or not cl.valid())
        {
            kos_handle_close(g_ep); // lone parked server or nothing spawned: nothing to drain
            tap::skip("pool too small");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        int32_t const dr_callrc = g_dr_callrc;
        int const dr_second = g_dr_second;
        TAP_CHECK(dr_callrc == 2);
        TAP_CHECK(dr_second == -KOS_EBADF);
    }

    // --- Call/reply: server dies mid-transaction -> caller EPIPE (teardown arm) ---
    // The server takes the call (REPLY_WAIT, holding the reply cap) then exits WITHOUT
    // replying. cap_teardown walks its table, hits the CAP_REPLY arm, and wakes the parked
    // caller with -KOS_EPIPE.
    Atomic<int32_t, Order::RELAXED> g_sd_callrc{-99};
    void sd_server(void*) // caps: done@1, E(WAIT)@2
    {
        char buf[16];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 2, 0, KOS_TIMEOUT_NONE);
        kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts); // takes the call, holds a reply cap
        info = opts.info;
        kos_sem_post(CH_DONE);                // report BEFORE exiting owning the cap
        kos_exit(0);                          // teardown EPIPEs the parked caller
    }
    void sd_caller(void*) // caps: done@1, E(SIGNAL)@2
    {
        char buf[8] = {0};
        g_sd_callrc = kos_call(2, buf, 4, sizeof(buf)); // woken -KOS_EPIPE when the server dies
        kos_sem_post(CH_DONE);
    }
    void t_call_server_death()
    {
        g_sd_callrc = -99;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto sv = kos::thread::create_caps(sd_server, nullptr, "sdS", 10, scaps, 2);
        kos::thread::Handle cl;
        if (sv.valid())
        {
            kos_sleep_ns(3000000ull); // let the server park in recv (fastpath call)
            cl = kos::thread::create_caps(sd_caller, nullptr, "sdC", 12, ccaps, 2);
        }
        if (not sv.valid() or not cl.valid())
        {
            kos_handle_close(g_ep); // lone parked server or nothing spawned: nothing to drain
            tap::skip("pool too small");
            return;
        }
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0); // server's WAIT cap already gone -> main's is the last
        int32_t const sd_callrc = g_sd_callrc;
        TAP_CHECK(sd_callrc == -KOS_EPIPE);
    }

    // --- Call/reply: server dies pre-pop -> caller refused (recv_holders -> 0) ---
    // The caller parks in SEND_WAIT (no receiver has popped it yet). MAIN holds the sole
    // WAIT cap; closing it drives recv_holders to 0, which drains send_waiters and refuses
    // the parked call: the pre-pop counterpart to the mid-transaction teardown above.
    Atomic<int32_t, Order::RELAXED> g_pp_callrc{-99};
    void pp_caller(void*) // caps: done@1, E(SIGNAL)@2
    {
        char buf[8] = {0};
        g_pp_callrc = kos_call(2, buf, 4, sizeof(buf)); // parks SEND_WAIT; woken refused
        kos_sem_post(CH_DONE);
    }
    void t_call_prepop_death()
    {
        g_pp_callrc = -99;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        // Caller gets SIGNAL only; MAIN is the sole WAIT holder. No server ever recvs.
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto cl = kos::thread::create_caps(pp_caller, nullptr, "ppC", 12, ccaps, 2);
        TAP_CHECK(cl.valid()); // spawn failure would hang the drain below
        kos_sleep_ns(3000000ull);               // let the caller park in SEND_WAIT
        TAP_CHECK(kos_handle_close(g_ep) == 0);  // last WAIT cap -> recv_holders 0 -> refuse the call
        wait_n(1);
        int32_t const pp_callrc = g_pp_callrc;
        TAP_CHECK(pp_callrc == -KOS_ECONNREFUSED);
    }

    // --- Call/reply: donation ordering (positive) --------------------------------
    // low(8) server, high(20) caller, medium(12) spoiler. On the fastpath call the server is
    // D1-boosted to the caller's prio, so the spoiler (which wakes WHILE the server holds
    // the transaction) cannot preempt: the reply reaches the caller ('c') before the spoiler
    // runs ('m'). Without donation the spoiler preempts the low server and 'm' precedes 'c'.
    uint64_t g_don_unit = 1000000ull;
    Atomic<int32_t, Order::RELAXED> g_don_rc{-99};
    char g_don_rpl[8];
    // Shared by the donation arms staged by duration, which run one at a time. The server's
    // boosted hold and the instant the medium spoiler falls due: the claim is that the spoiler
    // was ready by the end of that hold and still did not get the CPU. The donor's own call
    // seats the boost in these arms, so a spoiler due before the hold opened is still ready
    // inside it: the caller outranks the spoiler and wakes first.
    Window g_don_spin;
    Atomic<uint32_t, Order::RELAXED> g_don_spoiler_due{0};
    constexpr char DON_SPOILER_PRECOND[] =
        "the spoiler falling due before the server's boosted hold ends, which is the whole "
        "of what donation has to defeat";
    void don_stage_reset()
    {
        window_reset(g_don_spin);
        g_don_spoiler_due = 0;
    }
    void don_server(void*) // caps: done@1, lock@2, E(WAIT)@3
    {
        char buf[16];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 3, 0, KOS_TIMEOUT_NONE);
        kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts); // parks first (no senders); D1-boosted at the call
        info = opts.info;
        log_put('a');
        window_open(g_don_spin);
        mtx_spin(g_don_unit * 4); // hold the CPU past the spoiler's wake, at the boosted prio
        window_close(g_don_spin);
        log_put('r');
        if (info.reply_cap != KOS_CAP_NONE)
        {
            char rpl[8];
            memcpy(rpl, "pong!", 5);
            kos_reply(info.reply_cap, rpl, 5); // deflate + wake the caller (it preempts now)
        }
        kos_sem_post(CH_DONE);
    }
    void don_caller(void*) // caps: done@1, lock@2, E(SIGNAL)@3
    {
        char buf[8];
        kos_sleep_ns(g_don_unit * 2); // call after the server has parked in recv (fastpath)
        memcpy(buf, "req", 3);
        g_don_rc = kos_call(3, buf, 3, sizeof(buf));
        if (g_don_rc > 0)
        {
            size_t k = static_cast<size_t>(g_don_rc);
            if (k > sizeof(g_don_rpl)) { k = sizeof(g_don_rpl); }
            memcpy(g_don_rpl, buf, k);
        }
        log_put('c');
        kos_sem_post(CH_DONE);
    }
    void don_spoiler(void*) // caps: done@1, lock@2 (medium prio)
    {
        g_don_spoiler_due = stamp_due(g_don_unit * 3);
        kos_sleep_ns(g_don_unit * 3); // wake while the server holds the boosted transaction
        log_put('m');
        kos_sem_post(CH_DONE);
    }
    void t_call_donation()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        log_reset();
        g_don_unit = mtx_time_unit();
        don_stage_reset();
        g_don_rc = -99;
        memset(g_don_rpl, 0, sizeof(g_don_rpl));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto sv = kos::thread::create_caps(don_server, nullptr, "dnS", 8, scaps, 3);
        auto cl = kos::thread::create_caps(don_caller, nullptr, "dnC", 20, ccaps, 3);
        auto sp = kos::thread::create_caps(don_spoiler, nullptr, "dnM", 12, mcaps, 2);
        if (not sv.valid() or not cl.valid() or not sp.valid())
        {
            // Drain whoever spawned: each posts g_done (the caller drives the server through
            // its reply, the spoiler is timed).
            int n = 0;
            if (sv.valid()) { n++; }
            if (cl.valid()) { n++; }
            if (sp.valid()) { n++; }
            wait_n(n);
            kos_handle_close(g_ep);
            tap::skip("pool too small");
            return;
        }
        wait_n(3);
        uint32_t const don_spoiler_due = g_don_spoiler_due;
        if (not window_reached(g_don_spin, don_spoiler_due))
        {
            kos_handle_close(g_ep);
            skip_window_lost(DON_SPOILER_PRECOND, g_don_spin, don_spoiler_due);
            return;
        }
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(count('a') == 1 and count('r') == 1 and count('c') == 1 and count('m') == 1);
        int32_t const don_rc = g_don_rc;
        TAP_CHECK(don_rc == 5 and memcmp(g_don_rpl, "pong!", 5) == 0);
        TAP_CHECK(nth('r', 1) < nth('c', 1)); // reply delivered: the caller ran after the server replied
        TAP_CHECK(nth('c', 1) < nth('m', 1)); // DONATION: the reply reached the caller before the spoiler ran
    }

    // Unlock an unrelated mutex during an IPC transaction and verify the server
    // keeps its donation. Test both a caller waiting for reply and one waiting
    // to send. If priority falls to base(8), the ready spoiler(12) runs first.
    Atomic<int32_t, Order::RELAXED> g_dh_rc{-99};
    void dh_server(void*) // caps: done@1, lock@2, E(WAIT)@3, mutex@4
    {
        char buf[16];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        kos_mutex_lock(4);
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 3, 0, KOS_TIMEOUT_NONE);
        kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts); // parks first (no senders); D1-boosted at the call
        info = opts.info;
        log_put('a');
        window_open(g_don_spin);
        mtx_spin(g_don_unit * 4);  // hold the CPU past the spoiler's wake, at the boosted prio
        window_close(g_don_spin);
        kos_mutex_unlock(4);      // the recompute under test: the reply donor must hold the boost
        log_put('r');
        if (info.reply_cap != KOS_CAP_NONE)
        {
            char rpl[8];
            memcpy(rpl, "pong!", 5);
            kos_reply(info.reply_cap, rpl, 5);
        }
        kos_sem_post(CH_DONE);
    }
    void dh_caller(void*) // caps: done@1, lock@2, E(SIGNAL)@3
    {
        char buf[8];
        kos_sleep_ns(g_don_unit * 2); // call after the server has parked in recv (fastpath)
        memcpy(buf, "req", 3);
        g_dh_rc = kos_call(3, buf, 3, sizeof(buf));
        log_put('c');
        kos_sem_post(CH_DONE);
    }
    void dh_spoiler(void*) // caps: done@1, lock@2 (medium prio)
    {
        g_don_spoiler_due = stamp_due(g_don_unit * 3);
        kos_sleep_ns(g_don_unit * 3); // wake while the server holds the boosted transaction
        log_put('m');
        kos_sem_post(CH_DONE);
    }
    void t_call_donation_hold()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        log_reset();
        g_don_unit = mtx_time_unit();
        don_stage_reset();
        g_dh_rc = -99;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_t m = KOS_CAP_NONE;
        TAP_CHECK(kos_mutex_create(&m) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY},
                                 {m, CH_MTX}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto sv = kos::thread::create_caps(dh_server, nullptr, "dhS", 8, scaps, 4);
        auto cl = kos::thread::create_caps(dh_caller, nullptr, "dhC", 20, ccaps, 3);
        auto sp = kos::thread::create_caps(dh_spoiler, nullptr, "dhM", 12, mcaps, 2);
        if (not sv.valid() or not cl.valid() or not sp.valid())
        {
            int n = 0;
            if (sv.valid()) { n++; }
            if (cl.valid()) { n++; }
            if (sp.valid()) { n++; }
            wait_n(n);
            kos_handle_close(g_ep);
            kos_handle_close(m);
            tap::skip("pool too small");
            return;
        }
        wait_n(3);
        uint32_t const dh_spoiler_due = g_don_spoiler_due;
        if (not window_reached(g_don_spin, dh_spoiler_due))
        {
            kos_handle_close(g_ep);
            kos_handle_close(m);
            skip_window_lost(DON_SPOILER_PRECOND, g_don_spin, dh_spoiler_due);
            return;
        }
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(kos_handle_close(m) == 0);
        int32_t const dh_rc = g_dh_rc;
        TAP_CHECK(dh_rc == 5);
        TAP_CHECK(count('a') == 1 and count('r') == 1 and count('m') == 1);
        // The whole arm: the unlock's recompute did NOT deflate the server.
        TAP_CHECK(nth('r', 1) < nth('m', 1));
    }

    // Same again for the OTHER mint site: the caller parks in SEND_WAIT first and the
    // server's recv pops it, so the reply cap is minted from the server's own syscall. The
    // two sites link the donor independently.
    //
    // Staged by handoff on one semaphore, never by a duration. The caller and the spoiler both
    // outrank the server, so both are parked on it before the server first runs. A post goes to
    // the highest waiter: the first readies the caller, which preempts and calls while the
    // server is awake, and that slowpath is the arm's identity. The second, after the recv has
    // minted and boosted, readies the spoiler under the boost.
    kos_cap_t g_ds_go = KOS_CAP_NONE;
    void ds_server(void*) // caps: done@1, lock@2, E(WAIT)@3, mutex@4, go@5
    {
        char buf[16];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        kos_mutex_lock(4);
        log_put('s');
        kos_sem_post(5);
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 3, 0, KOS_TIMEOUT_NONE);
        kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts);
        info = opts.info;
        log_put('a');
        kos_sem_post(5);
        kos_mutex_unlock(4); // the recompute under test
        log_put('r');
        if (info.reply_cap != KOS_CAP_NONE)
        {
            char rpl[8];
            memcpy(rpl, "pong!", 5);
            kos_reply(info.reply_cap, rpl, 5);
        }
        kos_sem_post(CH_DONE);
    }
    void ds_caller(void*) // caps: done@1, lock@2, E(SIGNAL)@3, go@4
    {
        char buf[8];
        kos_sem_wait(4);
        log_put('k');
        memcpy(buf, "req", 3);
        g_dh_rc = kos_call(3, buf, 3, sizeof(buf));
        log_put('c');
        kos_sem_post(CH_DONE);
    }
    void ds_spoiler(void*) // caps: done@1, lock@2, go@3 (medium prio)
    {
        kos_sem_wait(3);
        log_put('m');
        kos_sem_post(CH_DONE);
    }
    void t_call_donation_slow()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        // Ask before spawning: the three workers wait on each other, so a partial set
        // cannot be drained and a guard after the spawns would hang rather than skip.
        if (not pool_can_host(3))
        {
            tap::skip("pool too small (3 interdependent workers)");
            return;
        }
        log_reset();
        g_dh_rc = -99;
        g_ds_go = KOS_CAP_NONE;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_t m = KOS_CAP_NONE;
        TAP_CHECK(kos_mutex_create(&m) == 0);
        if (kos_sem_create(0, &g_ds_go) != 0)
        {
            kos_handle_close(g_ep);
            kos_handle_close(m);
            tap::skip("semaphore pool too small");
            return;
        }
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY},
                                 {m, CH_MTX}, {g_ds_go, CH_FULL}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY},
                                 {g_ds_go, CH_FULL}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ds_go, CH_FULL}};
        // The two waiters first: the server must find both parked when it first runs.
        auto cl = kos::thread::create_caps(ds_caller, nullptr, "dsC", 20, ccaps, 4);
        auto sp = kos::thread::create_caps(ds_spoiler, nullptr, "dsM", 12, mcaps, 3);
        auto sv = kos::thread::create_caps(ds_server, nullptr, "dsS", 8, scaps, 5);
        TAP_CHECK(sv.valid() and cl.valid() and sp.valid());
        wait_n(3);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(kos_handle_close(m) == 0);
        TAP_CHECK(kos_handle_close(g_ds_go) == 0);
        int32_t const dh_rc = g_dh_rc;
        TAP_CHECK(dh_rc == 5);
        TAP_CHECK(count('s') == 1 and count('k') == 1 and count('a') == 1 and count('r') == 1
                  and count('m') == 1);
        TAP_CHECK(nth('s', 1) < nth('k', 1) and nth('k', 1) < nth('a', 1)); // called while awake
        TAP_CHECK(nth('r', 1) < nth('m', 1));
    }

    // Same shape, but the donor is a caller parked in SEND_WAIT rather than a reply cap:
    // the server takes and replies to a first call, so no reply cap is live, and the
    // caller's SECOND call arrives while the server is awake (slowpath -> D2 boost).
    Atomic<uint32_t, Order::RELAXED> g_dp_call2_at{0};
    void dp_server(void*) // caps: done@1, lock@2, E(WAIT)@3, mutex@4
    {
        char buf[16];
        struct kos_recv_info i1 = {0, KOS_CAP_NONE};
        kos_mutex_lock(4);
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 3, 0, KOS_TIMEOUT_NONE);
        kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts); // call #1, fastpath
        i1 = opts.info;
        if (i1.reply_cap != KOS_CAP_NONE)
        {
            char rpl[4];
            memcpy(rpl, "1", 1);
            kos_reply(i1.reply_cap, rpl, 1); // no reply cap live past here
        }
        log_put('a');
        window_open(g_don_spin);
        mtx_spin(g_don_unit * 4); // call #2 parks in SEND_WAIT here; the spoiler wakes here
        window_close(g_don_spin);
        kos_mutex_unlock(4);     // the recompute under test: the SEND_WAIT donor must hold it
        log_put('r');
        struct kos_recv_info i2 = {0, KOS_CAP_NONE};
        struct kos_reply_recv_opts opts2;
        kos_reply_recv_opts_init(&opts2, 3, 0, KOS_TIMEOUT_NONE);
        kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts2);
        i2 = opts2.info;
        if (i2.reply_cap != KOS_CAP_NONE)
        {
            char rpl[4];
            memcpy(rpl, "2", 1);
            kos_reply(i2.reply_cap, rpl, 1);
        }
        kos_sem_post(CH_DONE);
    }
    void dp_caller(void*) // caps: done@1, lock@2, E(SIGNAL)@3
    {
        char buf[8];
        kos_sleep_ns(g_don_unit * 2); // call #1 after the server has parked in recv
        memcpy(buf, "a", 1);
        int32_t const r1 = kos_call(3, buf, 1, sizeof(buf));
        memcpy(buf, "b", 1);
        // Call #2 is the donor: it has to reach SEND_WAIT inside the server's spin, or there
        // is no parked sender for the unlock's recompute to keep the boost from.
        g_dp_call2_at = stamp_now();
        int32_t const r2 = kos_call(3, buf, 1, sizeof(buf)); // server is awake -> slowpath
        g_dh_rc = r1 + r2;
        log_put('c');
        kos_sem_post(CH_DONE);
    }
    void t_call_donation_pending()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        log_reset();
        g_don_unit = mtx_time_unit();
        don_stage_reset();
        g_dp_call2_at = 0;
        g_dh_rc = -99;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_t m = KOS_CAP_NONE;
        TAP_CHECK(kos_mutex_create(&m) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY},
                                 {m, CH_MTX}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto sv = kos::thread::create_caps(dp_server, nullptr, "dpS", 8, scaps, 4);
        auto cl = kos::thread::create_caps(dp_caller, nullptr, "dpC", 20, ccaps, 3);
        auto sp = kos::thread::create_caps(dh_spoiler, nullptr, "dpM", 12, mcaps, 2);
        if (not sv.valid() or not cl.valid() or not sp.valid())
        {
            int n = 0;
            if (sv.valid()) { n++; }
            if (cl.valid()) { n++; }
            if (sp.valid()) { n++; }
            wait_n(n);
            kos_handle_close(g_ep);
            kos_handle_close(m);
            tap::skip("pool too small");
            return;
        }
        wait_n(3);
        uint32_t const dp_call2_at = g_dp_call2_at;
        uint32_t const dp_spoiler_due = g_don_spoiler_due;
        uint32_t const dp_spin_close = g_don_spin.close.load();
        // Only the CLOSING end, and the asymmetry is real: the caller outranks the server, so
        // it returns from call #1 and issues call #2 before the server resumes and opens its
        // spin at all. What the donor has to be is parked when the unlock recomputes, and the
        // unlock is the first thing past the spin.
        if (dp_call2_at == 0 or dp_spin_close == 0
            or not stamp_before(dp_call2_at, dp_spin_close))
        {
            kos_handle_close(g_ep);
            kos_handle_close(m);
            TAP_SKIP_VACUOUS("the second call did not reach SEND_WAIT before the unlock "
                             "recomputed, so this arm had no parked donor to keep the boost from");
            return;
        }
        if (not window_reached(g_don_spin, dp_spoiler_due))
        {
            kos_handle_close(g_ep);
            kos_handle_close(m);
            skip_window_lost(DON_SPOILER_PRECOND, g_don_spin, dp_spoiler_due);
            return;
        }
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(kos_handle_close(m) == 0);
        int32_t const dh_rc = g_dh_rc;
        TAP_CHECK(dh_rc == 2);
        TAP_CHECK(count('a') == 1 and count('r') == 1 and count('m') == 1);
        TAP_CHECK(nth('r', 1) < nth('m', 1));
    }

    // --- Bus service: per-device slot profiles ---------------------------
    // Gated for FLASH, not for a syscall: the mock backend plus serve_one cost ~1.3 KiB, and
    // the non-selftest bluepill-c8 image has ~1.3 KiB of its 64 KiB left. Every CI gate sets
    // the flag.
#if defined(KICKOS_ENABLE_SELFTEST)
    // A controller has a single live profile register set, so kickos::spi::serve_one keeps
    // one device HANDLE per kos_bus_req.device. The backend here is spi_mock.cc, which fills
    // the buffer with the word size of the handle it was given, so a transfer on slot 0 must
    // read back slot 0's word size even after slot 1 was opened. MAIN must be the server: a
    // spawned server plus a spawned client is two workers, which a 2-slot pool cannot host.

    // What the client observed, in call order.
    Atomic<int, Order::RELAXED> g_slot_cfg0{-99};
    Atomic<int, Order::RELAXED> g_slot_cfg1{-99};
    Atomic<int32_t, Order::RELAXED> g_slot_rx0{-99};
    Atomic<int32_t, Order::RELAXED> g_slot_rx1{-99};
    Atomic<int32_t, Order::RELAXED> g_slot_unconf{-99};
    Atomic<int32_t, Order::RELAXED> g_slot_oor{-99};
    unsigned char g_slot_b0[2] = {0, 0};
    unsigned char g_slot_b1[2] = {0, 0};

    // Frame + kos_call one XFER of `len` dummy bytes on slot `dev`; copies the reply's
    // rx bytes into `rx`. Returns the service's rx length, or a negative -KOS_E*.
    int32_t slot_xfer(uint8_t dev, size_t len, unsigned char* rx)
    {
        unsigned char buf[32];
        size_t const framing = sizeof(struct kos_bus_req) + sizeof(struct kos_bus_seg);
        struct kos_bus_req* req = reinterpret_cast<struct kos_bus_req*>(buf);
        req->proto = KOS_BUS_SPI;
        req->op = KOS_BUS_OP_XFER;
        req->device = dev;
        req->nseg = 1;
        req->region_cap = -1;
        req->offset = 0;
        struct kos_bus_seg* seg =
            reinterpret_cast<struct kos_bus_seg*>(buf + sizeof(struct kos_bus_req));
        seg->len = static_cast<uint16_t>(len);
        seg->flags = 0;
        seg->rsv = 0;
        memset(buf + framing, 0, len);

        int32_t const rc = kos_call(2, buf, framing + len, sizeof(buf));
        if (rc < 0)
        {
            return rc;
        }
        struct kos_bus_rsp const* rsp = reinterpret_cast<struct kos_bus_rsp const*>(buf);
        if (rsp->status < 0)
        {
            return rsp->status;
        }
        memcpy(rx, buf + sizeof(struct kos_bus_rsp), len);
        return rsp->len;
    }

    // Frame + kos_call one CONFIG on slot `dev` with `word_bits`.
    int slot_config(uint8_t dev, uint8_t word_bits)
    {
        unsigned char buf[32];
        struct kos_bus_req* req = reinterpret_cast<struct kos_bus_req*>(buf);
        req->proto = KOS_BUS_SPI;
        req->op = KOS_BUS_OP_CONFIG;
        req->device = dev;
        req->nseg = 0;
        req->region_cap = -1;
        req->offset = 0;
        struct kos_bus_cfg cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.hz = 1000000u;
        cfg.word_bits = word_bits;
        cfg.cs_policy = KOS_BUS_CS_NONE;
        memcpy(buf + sizeof(struct kos_bus_req), &cfg, sizeof(cfg));

        int32_t const rc = kos_call(2, buf, sizeof(struct kos_bus_req) + sizeof(cfg), sizeof(buf));
        if (rc < 0)
        {
            return static_cast<int>(rc);
        }
        struct kos_bus_rsp const* rsp = reinterpret_cast<struct kos_bus_rsp const*>(buf);
        return rsp->status;
    }

    void slot_client(void*) // caps: done@1, E(SIGNAL)@2
    {
        g_slot_cfg0 = slot_config(0, 8);
        g_slot_cfg1 = slot_config(1, 16);
        g_slot_rx0 = slot_xfer(0, sizeof(g_slot_b0), g_slot_b0);
        g_slot_rx1 = slot_xfer(1, sizeof(g_slot_b1), g_slot_b1);
        unsigned char sink[2];
        g_slot_unconf = slot_xfer(2, sizeof(sink), sink); // in range, never configured
        g_slot_oor = slot_xfer(KOS_BUS_DEV_MAX, sizeof(sink), sink); // out of range
        kos_sem_post(CH_DONE);
    }
    void t_bus_device_slots()
    {
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto cl = kos::thread::create_caps(slot_client, nullptr, "slot", 12, ccaps, 2,
                                           KOS_POLICY_FIFO, 0, /*privileged=*/false);
        if (not cl.valid())
        {
            kos_handle_close(g_ep);
            tap::skip("pool too small");
            return;
        }

        struct kos_spi_bus bus;
        // No line, so no notification and no bit: a polled engine never blocks.
        struct kos_spi_bus_config bcfg = {0u, KOS_CAP_NONE, KOS_CAP_NONE, KOS_CAP_NONE, 0u, 0u};
        TAP_CHECK(kos_spi_bus_open(&bus, &bcfg) == 0);
        kickos::spi::SlotTable slots;
        unsigned char msg[64]; // the six requests below are 24 bytes at most
        for (int i = 0; i < 6; i++)
        {
            struct kos_recv_info info = {0u, KOS_CAP_NONE};
            struct kos_reply_recv_opts opts;
            kos_reply_recv_opts_init(&opts, g_ep, 0, KOS_TIMEOUT_NONE);
            int32_t const n = kos_reply_recv(KOS_CAP_NONE, msg, kos_call_lens_pack(0, sizeof(msg)), &opts);
            info = opts.info;
            if (n < 0 or info.reply_cap == KOS_CAP_NONE)
            {
                break;
            }
            size_t const rlen = kickos::spi::serve_one(&bus, slots, msg, static_cast<size_t>(n));
            (void)kos_reply(info.reply_cap, msg, rlen);
        }
        wait_n(1);
        TAP_CHECK(kos_handle_close(g_ep) == 0);

        int const slot_cfg0 = g_slot_cfg0;
        int const slot_cfg1 = g_slot_cfg1;
        TAP_CHECK(slot_cfg0 == 0 and slot_cfg1 == 0);
        int32_t const slot_rx0 = g_slot_rx0;
        int32_t const slot_rx1 = g_slot_rx1;
        int32_t const slot_unconf = g_slot_unconf;
        int32_t const slot_oor = g_slot_oor;
        TAP_CHECK(slot_rx0 == 2 and g_slot_b0[0] == 8 and g_slot_b0[1] == 8); // slot 0 kept its own
        TAP_CHECK(slot_rx1 == 2 and g_slot_b1[0] == 16 and g_slot_b1[1] == 16); // slot 1 too
        TAP_CHECK(slot_unconf == -KOS_EINVAL); // no CONFIG for that slot: refused
        TAP_CHECK(slot_oor == -KOS_EINVAL); // slot >= KOS_BUS_DEV_MAX: refused
    }
#endif

#if defined(KICKOS_ENABLE_SELFTEST)
    // --- UART service: the wire ABI over the shared rings -----
    // serve_one touches only the shared block, never a register, so the whole request/reply
    // surface is testable with no device at all. MAIN is the server, so the client is the one
    // thread spawned. The TX doorbell is structurally NOT coverable here: serve_one rings
    // kos_irq_notify on child cap index 2, which in main's own table names another object.
    // Keep this at ZERO statics: a 1 KiB static would come out of the tiny boards' user arena
    // and turn a later mem_self_grant probe into a skip.
    struct UartResults
    {
        int wr;        // bytes the ring accepted
        int wr_big;    // a larger write, still inside the 512-byte ring
        int badframe;  // len claiming more than the frame carried
        int block;     // a blocking read
        int rd;        // bytes returned by READ
        int stats_tx;  // tx_bytes the driver counted
        int mode_bad;  // SET_MODE carrying an unknown bit
        int mode_clr;  // SET_MODE clearing the policy
        int mode_set;  // SET_MODE seating KOS_UART_F_NONBLOCK
        unsigned char rdbuf[4];
    };

    // The client's one message buffer, request and reply alike. NOT on its stack: where the
    // syscall dispatches on the caller's own stack (KICKOS_KERNEL_STACKS 0) a 1 KiB pool
    // stack keeps under 400 bytes above the trap red zone, and this buffer alone is 256.
    unsigned char g_uart_msg[KOS_EP_MSG_MAX];
    unsigned char* uart_payload()
    {
        return g_uart_msg + sizeof(struct kos_uart_req);
    }
    unsigned char const* uart_reply()
    {
        return g_uart_msg + sizeof(struct kos_uart_rsp);
    }

    // Frame + kos_call one request whose `carried` payload bytes are already at
    // uart_payload(); returns rsp.status, or rsp.len when status is 0, the reply payload
    // left at uart_reply().
    int uart_call(uint8_t op, uint8_t flags, uint16_t len, size_t carried)
    {
        struct kos_uart_req req;
        memset(&req, 0, sizeof(req));
        req.op = op;
        req.flags = flags;
        req.len = len;
        memcpy(g_uart_msg, &req, sizeof(req));
        int32_t const rc = kos_call(2, g_uart_msg, sizeof(req) + carried, sizeof(g_uart_msg));
        if (rc < 0)
        {
            return static_cast<int>(rc);
        }
        struct kos_uart_rsp rsp;
        memcpy(&rsp, g_uart_msg, sizeof(rsp));
        if (rsp.status < 0)
        {
            return rsp.status;
        }
        return static_cast<int>(rsp.len);
    }

    void uart_client(void*) // caps: done@1, E(SIGNAL)@2
    {
        UartResults r;
        memset(&r, 0, sizeof(r));
        memcpy(uart_payload(), "hi!\n", 4);
        r.wr = uart_call(KOS_UART_WRITE, 0, 4, 4);
        memset(uart_payload(), 'x', 200);
        r.wr_big = uart_call(KOS_UART_WRITE, 0, 200, 200);
        // len claims more than the frame carried: refused rather than reading past it.
        r.badframe = uart_call(KOS_UART_WRITE, 0, 8, 0);
        // A blocking read is refused explicitly, never answered with 0 bytes.
        r.block = uart_call(KOS_UART_READ, KOS_UART_F_BLOCK, 4, 0);
        r.rd = uart_call(KOS_UART_READ, 0, 4, 0);
        if (r.rd >= 0 and static_cast<size_t>(r.rd) <= sizeof(r.rdbuf))
        {
            memcpy(r.rdbuf, uart_reply(), static_cast<size_t>(r.rd));
        }
        if (uart_call(KOS_UART_STATS, 0, 0, 0) == static_cast<int>(sizeof(struct kos_uart_stats)))
        {
            struct kos_uart_stats s;
            kickos::console::stats_unpack(&s, uart_reply());
            r.stats_tx = static_cast<int>(kos_counter_load(&s.tx_bytes));
        }
        // Over the WIRE, not against console::mode_apply directly, so serve_one's dispatch
        // is covered. Accept LAST, so the server can assert the mode was stored.
        r.mode_bad = uart_call(KOS_UART_SET_MODE, 0x80, 0, 0);
        r.mode_clr = uart_call(KOS_UART_SET_MODE, 0, 0, 0);
        r.mode_set = uart_call(KOS_UART_SET_MODE, KOS_UART_F_NONBLOCK, 0, 0);
        // A PLAIN send (no reply cap), which is how the server tells this frame apart
        // from a request.
        (void)kos_send(2, &r, sizeof(r));
        kos_sem_post(CH_DONE);
    }

    void t_uart_service()
    {
#if defined(KICKOS_SELFTEST_NO_UART_SERVICE)
        // Pinned per board (see this app's CMakeLists): this arm's 1 KiB arena block would
        // starve the arena probes. The body below MUST stay compiled so the client entry
        // point keeps a referrer; only the ALLOCATION must not happen.
        tap::skip("pinned: the 1 KiB UART block is reserved for the arena probes here");
        return;
#endif
        // The shared block comes from the arena, not from .bss or the stack: main's stack
        // is 2 KiB on the smallest boards, and static would shrink the arena for
        // every later test.
        void* blk = kos_ram_alloc(sizeof(kickos::uart::Shared));
        if (blk == nullptr)
        {
            tap::skip("arena cannot spare the 1 KiB UART block, board too small");
            return;
        }
        TAP_CHECK(kos_mem_self_grant(blk, sizeof(kickos::uart::Shared), 0) == 0);
        kickos::uart::Shared* sh = static_cast<kickos::uart::Shared*>(blk);
        kickos::uart::shared_init(sh);
        // Stand in for the IRQ thread: put four bytes in the RX ring so the READ below
        // has something to return.
        unsigned char const rx[4] = {'R', 'X', 'o', 'k'};
        TAP_CHECK(kos_byte_ring_push(&sh->rx, rx, 4) == 4);

        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto cl = kos::thread::create_caps(uart_client, nullptr, "uartcl", 12, ccaps, 2,
                                           KOS_POLICY_FIFO, 0, /*privileged=*/false);
        if (not cl.valid())
        {
            kos_handle_close(g_ep);
            tap::skip("pool too small");
            return;
        }
        UartResults got;
        memset(&got, 0, sizeof(got));
        unsigned char msg[KOS_EP_MSG_MAX];
        // Bounded so a client dying mid-sequence cannot park main here. MUST cover every
        // request uart_client makes plus its results frame; too low deadlocks wait_n below.
        for (int i = 0; i < 12; i++)
        {
            struct kos_recv_info info = {0u, KOS_CAP_NONE};
            struct kos_reply_recv_opts opts;
            kos_reply_recv_opts_init(&opts, g_ep, 0, KOS_TIMEOUT_NONE);
            int32_t const n = kos_reply_recv(KOS_CAP_NONE, msg, kos_call_lens_pack(0, sizeof(msg)), &opts);
            info = opts.info;
            if (n < 0)
            {
                break;
            }
            if (info.reply_cap == KOS_CAP_NONE)
            {
                if (static_cast<size_t>(n) == sizeof(got))
                {
                    memcpy(&got, msg, sizeof(got));
                }
                break; // the results frame is the client's last word
            }
            // The CONSOLE posture every silicon driver runs. A null here refuses SET_MODE.
            size_t const rlen = kickos::uart::serve_one(sh, &sh->mode, msg,
                                                        static_cast<size_t>(n));
            (void)kos_reply(info.reply_cap, msg, rlen);
        }
        wait_n(1);
        TAP_CHECK(kos_handle_close(g_ep) == 0);

        TAP_CHECK(got.wr == 4);
        TAP_CHECK(got.wr_big == 200); // still fits the 512-byte ring
        TAP_CHECK(got.badframe == -KOS_EINVAL);
        TAP_CHECK(got.block == -KOS_ENOSYS); // refused, NOT answered with 0 bytes
        TAP_CHECK(got.rd == 4);
        TAP_CHECK(got.rdbuf[0] == 'R' and got.rdbuf[1] == 'X');
        TAP_CHECK(got.rdbuf[2] == 'o' and got.rdbuf[3] == 'k');
        // Counted, not merely present: 4 + 200 accepted bytes.
        TAP_CHECK(got.stats_tx == 204);
        TAP_CHECK(got.mode_bad == -KOS_EINVAL); // unknown bit refused whole
        TAP_CHECK(got.mode_clr == 0);
        TAP_CHECK(got.mode_set == 0);
        // Server-side read: the write serve_one made, not the reply it sent.
        TAP_CHECK(sh->mode == KOS_UART_F_NONBLOCK);
    }
#endif

#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
    // --- Bound-check: a recv/send pointer outside the caller's regions -> -KOS_EFAULT
    // The write-oracle / cross-domain-read is closed the same way as the console
    // buffer: an unprivileged caller cannot launder an un-owned page through IPC.
    Atomic<int32_t, Order::RELAXED> g_ep_badrecv_rc{-99};
    Atomic<int32_t, Order::RELAXED> g_ep_badsend_rc{-99};
    Atomic<int32_t, Order::RELAXED> g_ep_badopts_rc{-99};
    int g_ep_bnd_neg_ran = 0;
    void ep_bound_worker(void*) // caps: done@1, E@2 (unpriv)
    {
        void* bad = kos_guard_addr(); // an arena page granted to no domain
        if (bad != nullptr)
        {
            struct kos_reply_recv_opts opts;
            kos_reply_recv_opts_init(&opts, 2, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
            g_ep_badrecv_rc = kos_reply_recv(KOS_CAP_NONE, bad, kos_call_lens_pack(0, 8), &opts); // write oracle -> -KOS_EFAULT
            g_ep_badsend_rc = kos_send(2, static_cast<char const*>(bad), 8); // cross-domain read -> -KOS_EFAULT
            // The opts struct is IN-OUT, and an arena page is granule-aligned, so this
            // clears the alignment gate above and lands on the readable+writable check.
            // The message buffer is a valid stack local: the refusal is about opts alone.
            char obuf[8];
            g_ep_badopts_rc =
                kos_reply_recv(KOS_CAP_NONE, obuf, kos_call_lens_pack(0, sizeof(obuf)),
                               static_cast<struct kos_reply_recv_opts*>(bad));
            g_ep_bnd_neg_ran = 1;
        }
        kos_sem_post(CH_DONE);
    }
    void t_endpoint_bound()
    {
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        g_ep_badrecv_rc = -99; g_ep_badsend_rc = -99;
        g_ep_badopts_rc = -99;
        g_ep_bnd_neg_ran = 0;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, CH_FULL}};
        auto w = kos::thread::create_caps(ep_bound_worker, nullptr, "epbn", 12, caps, 2,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        wait_n(1);
        if (g_ep_bnd_neg_ran)
        {
            int32_t const ep_badrecv_rc = g_ep_badrecv_rc;
            int32_t const ep_badsend_rc = g_ep_badsend_rc;
            int32_t const ep_badopts_rc = g_ep_badopts_rc;
            TAP_CHECK(ep_badrecv_rc == -KOS_EFAULT); // bad recv buffer rejected, never parked
            TAP_CHECK(ep_badsend_rc == -KOS_EFAULT); // bad send buffer rejected, never parked
            TAP_CHECK(ep_badopts_rc == -KOS_EFAULT); // un-owned opts rejected, no deadline read
        }
        TAP_CHECK(kos_handle_close(g_ep) == 0);
    }
#endif

    // --- Cross-domain rendezvous under enforcement -------------------------------
    // main and an UNPRIVILEGED worker in a DIFFERENT memory domain rendezvous both ways: the
    // arriving side's kernel copy lands in the parked peer's domain, not the arriver's loaded
    // regions. The worker's payload buffer lives in its own granted domain region, and the
    // clean endpoint free at the end is what validates the delegation accounting.
    kos_cap_t g_xd_done = KOS_CAP_NONE; // PRIVATE completion sem: the worker posts it at CH_DONE, not the shared g_done
    // A round trip and not a pair of reports: the worker's memory grant makes it a task of
    // its own, so an app global it writes is its own copy. Its verdict comes back over the
    // endpoint.
    void xd_worker(void* arg) // caps: done@1, E(FULL)@2; arg = domain buffer
    {
        char* b = static_cast<char*>(arg);
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 2, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        int32_t const n = kos_reply_recv(KOS_CAP_NONE, b, kos_call_lens_pack(0, 8), &opts);
        int32_t verdict = n;
        for (int i = 0; i < 8; i++)
        {
            if (b[i] != static_cast<char>('a' + i))
            {
                verdict = -1;
            }
        }
        (void)kos_send(2, &verdict, sizeof(verdict));
        kos_sem_post(CH_DONE);
    }
    void t_endpoint_crossdomain()
    {
        void* wbuf = kos_ram_alloc(256);
        if (wbuf == nullptr)
        {
            tap::skip("arena cannot spare a domain region");
            return;
        }
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_sem_create(0, &g_xd_done); // PRIVATE: never satisfies another test's wait_n(g_done)
        kos_cap_grant wcaps[] = {{g_xd_done, CH_FULL}, {g_ep, CH_FULL}};
        auto w = kos::thread::create_caps(xd_worker, wbuf, "xdW", 12, wcaps, 2,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false, wbuf, 256);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            kos_handle_close(g_ep);
            kos_sem_destroy(g_xd_done);
            return;
        }
        char out[8];
        for (int i = 0; i < 8; i++)
        {
            out[i] = static_cast<char>('a' + i);
        }
        int32_t const sent = kos_send(g_ep, out, 8);
        int32_t verdict = -99;
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, g_ep, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        int32_t const got = kos_reply_recv(KOS_CAP_NONE, &verdict, kos_call_lens_pack(0, sizeof(verdict)), &opts);
        kos_sem_wait(g_xd_done); // this test's own completion sem, not the shared g_done
        // Reclaimed before the verdict: a TAP_CHECK returns on the spot, and an endpoint left
        // behind here starves every later arm that needs one.
        int const closed = kos_handle_close(g_ep); // both delegated caps torn down -> freed
        kos_sem_destroy(g_xd_done);
        TAP_CHECK(sent == 8);
        TAP_CHECK(got == static_cast<int32_t>(sizeof(verdict)));
        // 8 is the worker's own recv count, so this says the byte-exact payload crossed the
        // address-space boundary and the verdict crossed it back.
        TAP_CHECK(verdict == 8);
        TAP_CHECK(closed == 0);
    }

    // Allocate semaphores, then mutexes if their pool or task budget is exhausted.
    // This separates capability-table exhaustion (EMFILE) from object exhaustion
    // (ENOMEM/EAGAIN). Combined budgets must cover the table's free slots
    // for cap_index0 and cap_gen_reuse to reach EMFILE.
    int fill_one_cap_typed(kos_cap_t* out, bool* is_sem)
    {
        *is_sem = true;
        int rc = kos_sem_create(0, out);
        if (rc == -KOS_ENOMEM or rc == -KOS_EAGAIN)
        {
            *is_sem = false;
            rc = kos_mutex_create(out);
        }
        return rc;
    }

    int fill_one_cap(kos_cap_t* out)
    {
        bool is_sem = false;
        return fill_one_cap_typed(out, &is_sem);
    }

    // The low 16 bits of a cap handle are its table slot and the high 16 its cap-gen; the
    // split is fixed fleet-wide (cap.h), so neither is board-derived.
    constexpr kos_cap_t CAP_IDX_MASK = 0xFFFFu;
    constexpr int CAP_GEN_SHIFT = 16;

    // Own-creates until the table refuses, and closes on the way out whatever the arm did not
    // close itself: a failing TAP_CHECK returns from the middle of an arm, and a table left
    // full fails every later create for a reason that is not the one under test.
    struct TableFill
    {
        kos_cap_t held[KICKOS_MAX_HANDLES];
        bool is_sem[KICKOS_MAX_HANDLES];
        int n = 0;
        int stop = 0; // -KOS_EMFILE ends it on the table, -KOS_ENOMEM on an object pool

        TableFill()
        {
            while (n < static_cast<int>(KICKOS_MAX_HANDLES))
            {
                kos_cap_t h = KOS_CAP_NONE;
                bool sem = false;
                stop = fill_one_cap_typed(&h, &sem);
                if (stop != 0)
                {
                    return;
                }
                held[n] = h;
                is_sem[n] = sem;
                n = n + 1;
            }
        }
        TableFill(TableFill const&) = delete;
        TableFill& operator=(TableFill const&) = delete;
        ~TableFill()
        {
            for (int i = 0; i < n; i++)
            {
                if (held[i] != KOS_CAP_NONE)
                {
                    kos_handle_close(held[i]);
                }
            }
        }

        // Forgets the entry as well as closing it: the slot can be recycled onto a live object
        // before this goes out of scope, and a second close of a staled handle is refused.
        int close(int i)
        {
            int const rc = kos_handle_close(held[i]);
            held[i] = KOS_CAP_NONE;
            return rc;
        }
    };

    // main's free capability slots, or -1 where an object pool ran out before the table did.
    // The fill takes the free list whole and gives it back in the order it took it, so an
    // arm's own creates land where they would without the census.
    long cap_census()
    {
        TableFill fill;
        if (fill.stop != -KOS_EMFILE)
        {
            return -1;
        }
        return fill.n;
    }

    // --- Index 0 is the kernel stdout slot; an own create never lands there -------------
    void t_cap_index0()
    {
        // The reserved plane is never threaded onto the run's free list, so an own create
        // cannot pop a well-known slot (0 = console default, 1..FIRST_DYNAMIC-1 =
        // board/service delegation). Delegation seats an explicit index and so cannot catch a
        // free list built from a lower index; only an OWN create can.
        kos_cap_t s = KOS_CAP_NONE;
        TAP_CHECK(kos_sem_create(0, &s) == 0 and (s & CAP_IDX_MASK) >= KOS_CAP_FIRST_DYNAMIC);
        kos_cap_t e = KOS_CAP_NONE;
        TAP_CHECK(kos_endpoint_create(&e) == 0 and (e & CAP_IDX_MASK) >= KOS_CAP_FIRST_DYNAMIC);
        kos_cap_t m = KOS_CAP_NONE;
        TAP_CHECK(kos_mutex_create(&m) == 0 and (m & CAP_IDX_MASK) >= KOS_CAP_FIRST_DYNAMIC);
        TAP_CHECK(kos_handle_close(s) == 0);
        TAP_CHECK(kos_handle_close(e) == 0);
        TAP_CHECK(kos_handle_close(m) == 0);

        // Index 0 is the kernel stdout slot; BOTH of its postures are asserted here.
        // The discriminator is a ZERO-length send: a valid zero-length signal per
        // <kickos/sys.h>, which puts NO byte on the wire in either posture.
        int32_t const stdout_seated = kos_send(0, "", 0);
        if (stdout_seated == -KOS_EBADF)
        {
            // Pre-publish (g_stdout_target < 0): cap_install_defaults seats NOTHING at
            // index 0, so a send fails cleanly rather than resolving a stale object.
            TAP_CHECK(kos_send(0, "x", 1) == -KOS_EBADF);
        }
        else
        {
            // Post-publish: cap_seat_stdout put a send-only (CAP_SIGNAL) endpoint cap
            // at index 0, so the zero-length signal rendezvoused with the console
            // driver. A rendezvous only completes with a live receiver, so this is the
            // one place the suite proves console output is actually ACKed.
            TAP_CHECK(stdout_seated == 0);
        }

        // Exhaustion: own-creates fill the remaining slots [FIRST_DYNAMIC .. MAX_HANDLES-1]
        // and then fail with -KOS_EMFILE and NOT the -KOS_ENOMEM of an exhausted pool: the
        // reserved range stays off-limits even at the LAST free slot.
        TableFill fill;
        TAP_CHECK(fill.n >= 1);
        for (int i = 0; i < fill.n; i++)
        {
            // never a reserved slot, not even the last free one
            TAP_CHECK((fill.held[i] & CAP_IDX_MASK) >= KOS_CAP_FIRST_DYNAMIC);
        }
        kos_cap_t full = 0; // not KOS_CAP_NONE: the refusal must be what writes that word
        TAP_CHECK(fill_one_cap(&full) == -KOS_EMFILE // the TABLE names itself, not a pool
                  and full == KOS_CAP_NONE);
        full = 0;
        TAP_CHECK(fill_one_cap(&full) == -KOS_EMFILE // idempotent: still refused, no side effect
                  and full == KOS_CAP_NONE);
        for (int i = 0; i < fill.n; i++)
        {
            TAP_CHECK(fill.close(i) == 0);
        }
        kos_cap_t again = KOS_CAP_NONE; // table recovers once slots are freed
        TAP_CHECK(kos_sem_create(0, &again) == 0 and (again & CAP_IDX_MASK) != 0);
        TAP_CHECK(kos_handle_close(again) == 0);
    }

    // --- the segmented index decode: a live slot at or above the chunk granule ------------
    // A CONSISTENT bijective mis-decode in cap_slot relabels slots and every install/lookup
    // pair still agrees; a non-injective one breaks boot through cap_run_free_build instead.
    void t_cap_chunk_span()
    {
        // cmake/cap_geometry.cmake's target, reaching here as config/cap_width.h's
        // KCAP_CHUNK_TARGET. A table no wider than this compiles the FLAT decode and has no
        // segmented slot to reach, so a hardcoded mirror of the granule would make this arm
        // claim the segmented path on a board that never compiled it.
        constexpr uint32_t CHUNK_SLOTS = KICKOS_CAP_CHUNK_SLOTS;
        constexpr uint32_t TABLE_SLOTS = KICKOS_CAP_CHILD_WIDTH;

        TableFill fill;
        int const n = fill.n;
        TAP_CHECK(n >= 1);

        int top = 0;
        unsigned lowest = 0xffffu;
        for (int i = 0; i < n; i++)
        {
            unsigned const idx = static_cast<unsigned>(fill.held[i] & CAP_IDX_MASK);
            if (idx > static_cast<unsigned>(fill.held[top] & CAP_IDX_MASK))
            {
                top = i;
            }
            if (idx < lowest)
            {
                lowest = idx;
            }
            for (int j = i + 1; j < n; j++)
            {
                // A directory index that decoded to the wrong chunk would still report back
                // the index the install asked for, so only distinctness catches it.
                TAP_CHECK(idx != static_cast<unsigned>(fill.held[j] & CAP_IDX_MASK));
            }
        }

        // Gated on the CONFIGURED width, never on the index the fill reached: a wide board
        // whose pools ran dry early must FAIL here, not report PARTIAL.
        if (TABLE_SLOTS <= CHUNK_SLOTS)
        {
            for (int i = 0; i < n; i++)
            {
                TAP_CHECK(fill.close(i) == 0);
            }
            tap::partial("table is %u slot(s): the flat decode, no index reaches the granule",
                         static_cast<unsigned>(TABLE_SLOTS));
            return;
        }
        TAP_CHECK((fill.held[top] & CAP_IDX_MASK) >= CHUNK_SLOTS);
        if (lowest >= CHUNK_SLOTS)
        {
            // The refusal that ended the fill is what licenses the reading: -KOS_EMFILE means
            // it took every free slot, so nothing below the granule was free and the first
            // chunk is entirely seated by capabilities held for the life of the image.
            TAP_CHECK(fill.stop == -KOS_EMFILE);
            for (int i = 0; i < n; i++)
            {
                TAP_CHECK(fill.close(i) == 0);
            }
            tap::partial("the first chunk of %u is seated whole; own creates start at %u",
                         static_cast<unsigned>(CHUNK_SLOTS), lowest);
            return;
        }

        // Usable, not merely numbered: reaching the object is the only proof the directory
        // index and the in-chunk offset recombined onto the entry the install wrote.
        kos_cap_t const high = fill.held[top];
        if (fill.is_sem[top])
        {
            TAP_CHECK(kos_sem_post(high) == 0);
            TAP_CHECK(kos_sem_wait(high) == 0);
        }
        else
        {
            TAP_CHECK(kos_mutex_lock(high) == 0);
            TAP_CHECK(kos_mutex_unlock(high) == 0);
        }
        TAP_CHECK(fill.close(top) == 0);
        TAP_CHECK(kos_handle_close(high) == -KOS_EBADF); // the entry the close emptied
        for (int i = 0; i < n; i++)
        {
            if (i == top)
            {
                continue;
            }
            // Ordered after the close above on purpose: had the high slot's decode aliased
            // one of these, that close would have emptied this entry too and this would be
            // -KOS_EBADF.
            TAP_CHECK(fill.close(i) == 0);
        }
    }

    // --- the cap-gen half of the handle codec: a recycled slot stales the old handle -------
    void t_cap_gen_reuse()
    {
        TableFill fill;
        TAP_CHECK(fill.n >= 1);
        // The table has to be FULL, and -KOS_EMFILE is the only thing that says so. A close
        // then leaves the released slot as the free list's ONLY node, forcing the next install
        // back onto that index; with any slot still free the mint lands elsewhere (a release
        // goes to the TAIL, cap.h) and the cap-gen test stays unreachable.
        kos_cap_t refused = 0; // not KOS_CAP_NONE: the refusal must be what writes that word
        TAP_CHECK(fill_one_cap(&refused) == -KOS_EMFILE and refused == KOS_CAP_NONE);

        int const last = fill.n - 1;
        kos_cap_t const stale = fill.held[last];
        TAP_CHECK(fill.close(last) == 0);
        kos_cap_t fresh = KOS_CAP_NONE;
        bool fresh_is_sem = false;
        TAP_CHECK(fill_one_cap_typed(&fresh, &fresh_is_sem) == 0);
        TAP_CHECK((fresh & CAP_IDX_MASK) == (stale & CAP_IDX_MASK));     // the same slot
        TAP_CHECK((fresh >> CAP_GEN_SHIFT) != (stale >> CAP_GEN_SHIFT)); // a new cap-gen

        // The slot is in range and NOT empty, so the cap-gen comparison is the only test in
        // cap_lookup left that can refuse `stale`. `fresh` on that same index is the control:
        // without it a refusal for any other reason would read identically.
        if (fresh_is_sem)
        {
            TAP_CHECK(kos_sem_post(stale) == -KOS_EBADF);
            TAP_CHECK(kos_sem_post(fresh) == 0);
            TAP_CHECK(kos_sem_wait(fresh) == 0);
        }
        else
        {
            TAP_CHECK(kos_mutex_lock(stale) == -KOS_EBADF);
            TAP_CHECK(kos_mutex_lock(fresh) == 0);
            TAP_CHECK(kos_mutex_unlock(fresh) == 0);
        }
        TAP_CHECK(kos_handle_close(stale) == -KOS_EBADF);
        TAP_CHECK(kos_handle_close(fresh) == 0);
        for (int i = 0; i < last; i++)
        {
            TAP_CHECK(fill.close(i) == 0);
        }
    }

    // --- Per-task table width, and the inbound reply bound ------------------------
    //
    // No new file-scope state below: every worker reports through the shared event log, and
    // a static here would come straight out of the 16 KiB boards' user arena.

    void* units(unsigned n)
    {
        return reinterpret_cast<void*>(static_cast<uintptr_t>(n));
    }
    uint64_t unit_delay(void* arg)
    {
        return g_call_unit * static_cast<uint64_t>(reinterpret_cast<uintptr_t>(arg));
    }

    // Marks one '#' per capability it got: the width it was seated with, minus the reserved
    // plane, minus the one grant landing on a dynamic index (done@1 is below
    // KOS_CAP_FIRST_DYNAMIC and so was never on the free list, lock@2 is above it and was).
    void width_child(void*) // caps: done@1, lock@2
    {
        kos_cap_t held[KICKOS_MAX_HANDLES];
        int n = 0;
        while (n < static_cast<int>(sizeof(held) / sizeof(held[0])))
        {
            kos_cap_t h = KOS_CAP_NONE;
            if (fill_one_cap(&h) != 0)
            {
                break;
            }
            held[n] = h;
            n = n + 1;
        }
        kos_cap_t refused = 0; // not KOS_CAP_NONE: the refusal must be what writes that word
        if (fill_one_cap(&refused) == -KOS_EMFILE and refused == KOS_CAP_NONE)
        {
            log_put('E'); // the TABLE refused, not an object pool
        }
        for (int i = 0; i < n; i++)
        {
            if (kos_handle_close(held[i]) == 0)
            {
                log_put('#');
            }
        }
        kos_sem_post(CH_DONE);
    }

    // --- main's table and its child's are both KICKOS_CAP_CHILD_WIDTH wide ----------------
    void t_cap_child_width()
    {
        {
            // The init's delegations land from KOS_SPAWN_DELEGATED_CAP0 and own-creates from
            // KOS_CAP_FIRST_DYNAMIC; g_lock and g_done are main's two.
            uint32_t seated = KOS_SPAWN_DELEGATED_CAP0 + g_self->cap_grant_count;
            if (seated < KOS_CAP_FIRST_DYNAMIC)
            {
                seated = KOS_CAP_FIRST_DYNAMIC;
            }
            TableFill fill;
            tap::diag("main's table: %u seated, 2 shared, %d filled to %d, of %u",
                      static_cast<unsigned>(seated), fill.n, fill.stop,
                      static_cast<unsigned>(KICKOS_CAP_CHILD_WIDTH));
            TAP_CHECK(fill.stop == -KOS_EMFILE);
            TAP_CHECK(seated + 2u + static_cast<uint32_t>(fill.n) == KICKOS_CAP_CHILD_WIDTH);
        }
        if (not pool_can_host(1))
        {
            tap::skip("pool too small");
            return;
        }
        log_reset();
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto w = kos::thread::create_caps(width_child, nullptr, "cw", 10, caps, 2);
        TAP_CHECK(w.valid());
        wait_n(1);
        TAP_CHECK(count('E') == 1);
        TAP_CHECK(count('#') == KICKOS_CAP_CHILD_WIDTH - KOS_CAP_FIRST_DYNAMIC - 1);
    }

    // Holds A's reply capability across a SECOND recv, so B's call meets the bound. main's
    // first plain send is what unparks that second recv; the reply to A follows it, and that
    // is what lifts the bound for B's retry. main's second plain send ends the run.
    //
    // Both of B's calls are placed by sleep against a window the server owns, so the arm can
    // only judge the bound while B's first call was outstanding INSIDE the life of A's reply
    // capability and B's second one landed after it and before main ended the run. The
    // server stamps that life; B and main stamp their own instants.
    Window g_rb_live;
    Atomic<uint32_t, Order::RELAXED> g_rb_b1_at{0};
    Atomic<uint32_t, Order::RELAXED> g_rb_b2_at{0};
    Atomic<uint32_t, Order::RELAXED> g_rb_end_at{0};
    void rb_server(void* arg) // caps: done@1, lock@2, E(WAIT)@3
    {
        char b[8];
        kos_sleep_ns(unit_delay(arg));
        struct kos_recv_info first = {0, KOS_CAP_NONE};
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, CH_AUX, 0, KOS_TIMEOUT_NONE);
        kos_reply_recv(KOS_CAP_NONE, b, kos_call_lens_pack(0, sizeof(b)), &opts);
        first = opts.info;
        window_open(g_rb_live); // A's reply capability exists from this recv
        log_put('1');
        struct kos_recv_info wake = {0, KOS_CAP_NONE};
        struct kos_reply_recv_opts opts2;
        kos_reply_recv_opts_init(&opts2, CH_AUX, 0, KOS_TIMEOUT_NONE);
        kos_reply_recv(KOS_CAP_NONE, b, kos_call_lens_pack(0, sizeof(b)), &opts2);
        wake = opts2.info;
        window_close(g_rb_live); // and stops existing at the reply on the next line
        kos_reply(first.reply_cap, "r", 1);
        log_put('2');
        int plains = 0;
        if (wake.reply_cap == KOS_CAP_NONE)
        {
            plains = 1;
        }
        else
        {
            // Only a kernel with no bound puts a call here. Reply to it, and keep serving:
            // the arm must FAIL on the missing refusal, never hang on a stranded caller.
            kos_reply(wake.reply_cap, "r", 1);
        }
        while (plains < 2)
        {
            struct kos_recv_info info = {0, KOS_CAP_NONE};
            struct kos_reply_recv_opts opts3;
            kos_reply_recv_opts_init(&opts3, CH_AUX, 0, KOS_TIMEOUT_NONE);
            kos_reply_recv(KOS_CAP_NONE, b, kos_call_lens_pack(0, sizeof(b)), &opts3);
            info = opts3.info;
            if (info.reply_cap == KOS_CAP_NONE)
            {
                plains = plains + 1;
                continue;
            }
            kos_reply(info.reply_cap, "r", 1);
        }
        kos_sem_post(CH_DONE);
    }
    void rb_caller_a(void* arg) // caps: done@1, lock@2, E(SIGNAL)@3
    {
        char b[8] = {0};
        kos_sleep_ns(unit_delay(arg));
        if (kos_call(CH_AUX, b, 4, sizeof(b)) == 1)
        {
            log_put('A');
        }
        kos_sem_post(CH_DONE);
    }
    void rb_caller_b(void* arg) // caps: done@1, lock@2, E(SIGNAL)@3
    {
        char b[8] = {0};
        kos_sleep_ns(unit_delay(arg));
        g_rb_b1_at = stamp_now();
        if (kos_call(CH_AUX, b, 4, sizeof(b)) == -KOS_EMFILE)
        {
            log_put('E'); // refused against the SERVER's reply bound, not against our table
        }
        kos_sleep_ns(g_call_unit * 12); // past main's first plain send, so A has been replied to
        g_rb_b2_at = stamp_now();
        if (kos_call(CH_AUX, b, 4, sizeof(b)) == 1)
        {
            log_put('K'); // and admitted once A's reply capability was consumed
        }
        kos_sem_post(CH_DONE);
    }

    // `server_delay` decides WHICH probe refuses B. 0 parks the server in recv before either
    // caller runs, so B meets endpoint_call's fastpath probe; a delay past both callers makes
    // B park in CALL_SEND_WAIT first, so the refusal comes back through the recv-side scan.
    void reply_bound_arm(unsigned server_delay, unsigned b_delay)
    {
        // The choreography holds exactly ONE reply capability live and asserts B meets the
        // bound against it. A higher bound needs KICKOS_CAP_REPLY_MAX callers parked before
        // B, which is that many more thread slots and an ordering this log cannot express, so
        // skip rather than assert a bound that is not the configured one.
        if (KICKOS_CAP_REPLY_MAX != 1)
        {
            tap::skip("KICKOS_CAP_REPLY_MAX is %u: this arm only drives a bound of 1",
                      static_cast<unsigned>(KICKOS_CAP_REPLY_MAX));
            return;
        }
        if (not pool_can_host(3))
        {
            tap::skip("pool too small (3 interdependent workers)");
            return;
        }
        log_reset();
        g_call_unit = mtx_time_unit();
        window_reset(g_rb_live);
        g_rb_b1_at = 0;
        g_rb_b2_at = 0;
        g_rb_end_at = 0;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto sv = kos::thread::create_caps(rb_server, units(server_delay), "rbS", 8, scaps, 3);
        auto ca = kos::thread::create_caps(rb_caller_a, units(1), "rbA", 20, ccaps, 3);
        auto cb = kos::thread::create_caps(rb_caller_b, units(b_delay), "rbB", 12, ccaps, 3);
        TAP_CHECK(sv.valid() and ca.valid() and cb.valid());
        char plain[4] = {0};
        kos_sleep_ns(g_call_unit * (server_delay + 6));
        kos_send(g_ep, plain, 4); // unpark the second recv, so the server can reply to A
        kos_sleep_ns(g_call_unit * 14);
        g_rb_end_at = stamp_now();
        kos_send(g_ep, plain, 4); // end the run, whatever the server ended up serving
        wait_n(3);
        uint32_t const rb_b1_at = g_rb_b1_at;
        uint32_t const rb_b2_at = g_rb_b2_at;
        uint32_t const rb_end_at = g_rb_end_at;
        uint32_t const rb_live_open = g_rb_live.open.load();
        uint32_t const rb_live_close = g_rb_live.close.load();
        if (rb_b1_at == 0 or rb_b2_at == 0 or rb_live_open == 0 or rb_live_close == 0)
        {
            kos_handle_close(g_ep);
            tap::skip("the run never reached the point where A's reply capability is both "
                      "minted and consumed, so there is no bound to meet");
            return;
        }
        // The fast arm's refusal comes from endpoint_call's own probe, which B only reaches
        // once the server is parked in recv holding A's capability; the slow arm's comes
        // from the recv-side scan, which B reaches by parking BEFORE that. So only the
        // closing end is common to the two.
        if (server_delay == 0 and not stamp_before(rb_live_open, rb_b1_at))
        {
            kos_handle_close(g_ep);
            TAP_SKIP_VACUOUS("B called %u us BEFORE A's reply capability was minted, so it "
                             "never met endpoint_call's fastpath probe against a live one",
                             static_cast<unsigned>((rb_live_open - rb_b1_at) / 1000u));
            return;
        }
        if (not stamp_before(rb_b1_at, rb_live_close))
        {
            kos_handle_close(g_ep);
            TAP_SKIP_VACUOUS("B called %u us AFTER A's reply capability was released, so the "
                             "bound it met was not the one this arm stages",
                             static_cast<unsigned>((rb_b1_at - rb_live_close) / 1000u));
            return;
        }
        if (not stamp_before(rb_live_close, rb_b2_at) or not stamp_before(rb_b2_at, rb_end_at))
        {
            kos_handle_close(g_ep);
            TAP_SKIP_VACUOUS("B's retry did not fall between the release of A's capability and "
                             "main's send ending the run, so its admission is not the bound "
                             "lifting");
            return;
        }
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(count('E') == 1);           // B refused while A's reply capability was live
        TAP_CHECK(count('K') == 1);           // and admitted after it was consumed
        TAP_CHECK(count('A') == 1);           // A was never crowded out by B
        TAP_CHECK(nth('E', 1) < nth('2', 1)); // the refusal preceded the reply that lifted it
    }

    // --- the reply bound, met at endpoint_call's fastpath probe ---------------------------
    void t_cap_reply_bound_fast()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        reply_bound_arm(0, 3);
    }

    // --- the same bound, delivered through the recv-side scan of parked callers -----------
    void t_cap_reply_bound_slow()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        reply_bound_arm(4, 0);
    }

    // Serves calls until main's plain send ends the run, which main makes once every caller has
    // finished. arg != 0 consumes the FIRST reply capability with kos_handle_close rather than
    // kos_reply: a release path kos_reply does not cover, and the caller sees -KOS_EPIPE.
    void rp_server(void* arg) // caps: done@1, lock@2, E(WAIT)@3
    {
        char b[8];
        for (int i = 0; i < 3; i++)
        {
            struct kos_recv_info info = {0, KOS_CAP_NONE};
            struct kos_reply_recv_opts opts;
            kos_reply_recv_opts_init(&opts, CH_AUX, 0, STALL_TOLERANT_US);
            int32_t const n =
                kos_reply_recv(KOS_CAP_NONE, b, kos_call_lens_pack(0, sizeof(b)), &opts);
            info = opts.info;
            if (n < 0 or info.reply_cap == KOS_CAP_NONE)
            {
                break; // main's plain send: the run is over, refused callers and all
            }
            if (i == 0 and arg != nullptr)
            {
                kos_handle_close(info.reply_cap);
                log_put('c');
                continue;
            }
            log_put('K');
            kos_reply(info.reply_cap, "r", 1);
        }
        kos_sem_post(CH_DONE);
    }
    void rp_caller(void* arg) // caps: done@1, lock@2, E(SIGNAL)@3
    {
        char b[8] = {0};
        kos_sleep_ns(unit_delay(arg));
        int32_t const rc = kos_call_timed(CH_AUX, b, 4, sizeof(b), STALL_TOLERANT_US);
        if (rc == -KOS_EPIPE)
        {
            log_put('P');
        }
        if (rc == 1)
        {
            log_put('B');
        }
        kos_sem_post(CH_DONE);
    }

    // --- a reply cap consumed by CLOSE still admits the next caller -----------------------
    void t_cap_reply_release_close()
    {
        if (not pool_can_host(3))
        {
            tap::skip("pool too small (3 interdependent workers)");
            return;
        }
        log_reset();
        g_call_unit = mtx_time_unit();
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto sv = kos::thread::create_caps(rp_server, units(1), "rpS", 8, scaps, 3);
        auto ca = kos::thread::create_caps(rp_caller, units(1), "rpA", 20, ccaps, 3);
        auto cb = kos::thread::create_caps(rp_caller, units(5), "rpB", 12, ccaps, 3);
        TAP_CHECK(sv.valid() and ca.valid() and cb.valid());
        TAP_CHECK(ca.join(STALL_TOLERANT_US) == 0 and cb.join(STALL_TOLERANT_US) == 0);
        char plain[4] = {0};
        (void)kos_send_timed(g_ep, plain, 4, STALL_TOLERANT_US);
        wait_n(3);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(count('c') == 1 and count('P') == 1); // A's cap closed, A woken -KOS_EPIPE
        TAP_CHECK(count('K') == 1 and count('B') == 1); // and B admitted behind it
    }

    // Takes a call and EXITS holding the reply capability: the teardown sweep is what has to
    // account it, or the slot's next occupant refuses its own first caller.
    void rr_dying_server(void*) // caps: done@1, lock@2, E(WAIT)@3
    {
        char b[8];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, CH_AUX, 0, STALL_TOLERANT_US);
        kos_reply_recv(KOS_CAP_NONE, b, kos_call_lens_pack(0, sizeof(b)), &opts);
        info = opts.info;
        log_put('d');
        kos_sem_post(CH_DONE);
    }

    // --- a slot reclaimed from a server that died mid-call admits its next caller ---------
    void t_cap_reply_slot_reuse()
    {
        if (not pool_can_host(2))
        {
            tap::skip("pool too small (2 interdependent workers)");
            return;
        }
        log_reset();
        g_call_unit = mtx_time_unit();
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        auto s1 = kos::thread::create_caps(rr_dying_server, nullptr, "rrD", 8, scaps, 3);
        auto c1 = kos::thread::create_caps(rp_caller, units(2), "rr1", 12, ccaps, 3);
        TAP_CHECK(s1.valid() and c1.valid());
        wait_n(2);
        TAP_CHECK(count('d') == 1 and count('P') == 1); // died holding a live reply cap

        // Both slots are EXITED now, so these two reclaim them.
        auto s2 = kos::thread::create_caps(rp_server, nullptr, "rrS", 8, scaps, 3);
        auto c2 = kos::thread::create_caps(rp_caller, units(2), "rr2", 12, ccaps, 3);
        TAP_CHECK(s2.valid() and c2.valid());
        TAP_CHECK(c2.join(STALL_TOLERANT_US) == 0);
        char plain[4] = {0};
        (void)kos_send_timed(g_ep, plain, 4, STALL_TOLERANT_US);
        wait_n(2);
        TAP_CHECK(kos_handle_close(g_ep) == 0);
        TAP_CHECK(count('K') == 1 and count('B') == 1); // the next occupant admitted its first
    }

    // --- console_publish needs AUTH_CONSOLE; a bad cap is rejected with no side effect ---
    int g_pub_rc = -99;
    void pub_denied_worker(void*) // caps: done@1
    {
        // Unprivileged caller: rejected before any console state change, so this never
        // actually hands over the console. The rest of the suite keeps printing.
        g_pub_rc = kos_console_publish(1, KOS_TASK_NONE);
        kos_sem_post(CH_DONE);
    }
    void t_console_publish()
    {
        // From main, which holds AUTH_CONSOLE: a bad/stale cap is rejected before the
        // deinit/flip, so console ownership stays as the composition set it. This
        // test never publishes anything itself.
        // The -KOS_EBADF assertions are exact: the AUTH_CONSOLE gate runs before the cap
        // resolve, so a main that lost the bit answers -KOS_EPERM and this test fails.
        TAP_CHECK(kos_console_publish(KOS_CAP_NONE, KOS_TASK_NONE) == -KOS_EBADF);
        TAP_CHECK(kos_console_publish(0x7fffffff, KOS_TASK_NONE) == -KOS_EBADF);
        // Unprivileged child: the privileged-only gate rejects it.
        g_pub_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto w = kos::thread::create_caps(pub_denied_worker, nullptr, "pubden", 10, caps, 1);
        TAP_CHECK(w.valid());
        wait_n(1);
        TAP_CHECK(g_pub_rc == -KOS_EPERM);
    }

    // --- a console published through HANDOUT for a task, given back when that task ends -----
    // main's own stdout is the endpoint it publishes, so from each publish until the task's end
    // main writes nothing: a TAP line there would park main on a console nobody receives on.
    // Each arm's own lines, and every line after, reach the wire only once the kernel console is
    // back, which is what the stream gate counts.
    constexpr uint32_t CON_KEPT = KOS_CAP_SIGNAL | KOS_CAP_TRANSFER | KOS_CAP_HANDOUT;
    constexpr uint32_t CON_PARK_US = 1000;
    constexpr uint32_t CON_SEND_US = 1000000;
    constexpr uint32_t CON_JOIN_US = 60000;
    kos_cap_t g_con_ep = KOS_CAP_NONE;
    // Takes one message and exits.
    void con_driver(void*) // caps: the console endpoint@1, WAIT only
    {
        char got[4];
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, KOS_SPAWN_DELEGATED_CAP0, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        (void)kos_reply_recv(KOS_CAP_NONE, got, kos_call_lens_pack(0, sizeof(got)), &o);
    }

    // The driver task's entry: it holds no right on the console and lives until main lets it go.
    void con_keeper(void*) // caps: hold@1
    {
        (void)kos_sem_wait(KOS_SPAWN_DELEGATED_CAP0);
    }

    // A stdout client spawned after the publish, so its stdout is the console's endpoint. In
    // main's task, so main reads what its send answered: a driver task may have its own space.
    int32_t g_con_sent = -99;
    void con_writer(void*)
    {
        g_con_sent = kos_send(KOS_CAP_STDOUT, "hi", 2);
    }

    struct ConNonblock
    {
        size_t tried = 99;
        int32_t sent = -99;
        bool set = false;
        bool read_back = false;
        bool would_block = false;
        bool cleared = false;
        bool main_blocking = false;
        int32_t main_sent = -99;
        bool fcntl_refusals = false;
    };

    ConNonblock g_nb;

    // O_NONBLOCK is the task's: set here, in a task of its own, it leaves main's blocking.
    bool con_nonblock_set(bool on)
    {
#if not KICKOS_LIBC_REENT
        // The host's own fcntl and write would act on the harness's stdout.
        int op = KOS_NONBLOCK_CLEAR;
        if (on)
        {
            op = KOS_NONBLOCK_SET;
        }
        return kos_task_nonblock(op) == static_cast<int>(on);
#else
        int flags = 0;
        if (on)
        {
            flags = O_NONBLOCK;
        }
        return fcntl(1, F_SETFL, flags) == 0;
#endif
    }

    bool con_nonblocking()
    {
#if not KICKOS_LIBC_REENT
        return kos_task_nonblock(KOS_NONBLOCK_GET) == 1;
#else
        return (fcntl(1, F_GETFL) & O_NONBLOCK) != 0;
#endif
    }

    // What the probe saw, carried out in its task's exit status: a task of its own may have its
    // own copy of this image's data.
    constexpr int NB_RAN = 1 << 6;
    constexpr int NB_SET = 1 << 0;
    constexpr int NB_READ_BACK = 1 << 1;
    constexpr int NB_SENT = 1 << 2;
    constexpr int NB_TRIED = 1 << 3;
    constexpr int NB_WOULD_BLOCK = 1 << 4;
    constexpr int NB_CLEARED = 1 << 5;

    // Non-blocking writes against a console whose task lives and nobody receives on: nothing is
    // taken and the writer carries on at once. Nothing before the first post can wait.
    void con_nb_probe(void*) // caps: go@1, back@2
    {
        char const probe = 'x';
        int seen = NB_RAN;
        if (con_nonblock_set(true))
        {
            seen |= NB_SET;
        }
        if (con_nonblocking())
        {
            seen |= NB_READ_BACK;
        }
        (void)kos_sem_post(KOS_SPAWN_DELEGATED_CAP0 + 1);
        (void)kos_sem_wait(KOS_SPAWN_DELEGATED_CAP0);
        if (kos_send(KOS_CAP_STDOUT, &probe, 1) == -KOS_ETIMEDOUT)
        {
            seen |= NB_SENT;
        }
        if (kickos::stdout_write(&probe, 1) == 0u)
        {
            seen |= NB_TRIED;
        }
#if not KICKOS_LIBC_REENT
        seen |= NB_WOULD_BLOCK;
#else
        errno = 0;
        int const wrote = static_cast<int>(write(1, &probe, 1));
        if (wrote == -1 and errno == EAGAIN)
        {
            seen |= NB_WOULD_BLOCK;
        }
#endif
        (void)con_nonblock_set(false);
        if (not con_nonblocking())
        {
            seen |= NB_CLEARED;
        }
        kos_exit(seen);
    }

    // fcntl knows fds 0 to 2 and F_GETFL, F_SETFL, F_GETFD, F_SETFD, and refuses the rest.
    bool con_fcntl_refusals()
    {
#if not KICKOS_LIBC_REENT
        return true;
#else
        errno = 0;
        bool const bad_fd = fcntl(3, F_GETFL) == -1 and errno == EBADF;
        errno = 0;
        bool const bad_cmd = fcntl(1, F_DUPFD, 3) == -1 and errno == EINVAL;
        bool const getfd = fcntl(2, F_GETFD) == 0;
        return bad_fd and bad_cmd and getfd and (fcntl(0, F_GETFL) & O_ACCMODE) == O_RDONLY;
#endif
    }

    // main's side: while the probe's task is non-blocking, main's is not, and its send parks.
    void con_nonblock()
    {
        g_nb = ConNonblock{};
        g_nb.fcntl_refusals = con_fcntl_refusals();
        kos_task_t nb_task = KOS_TASK_NONE;
        kos_cap_t go = KOS_CAP_NONE;
        kos_cap_t back = KOS_CAP_NONE;
        if (kos_task_create(nullptr, 0, 0, &nb_task) != 0 or kos_sem_create(0, &go) != 0
            or kos_sem_create(0, &back) != 0)
        {
            return;
        }
        kos_cap_grant const caps[] = {{go, CH_FULL}, {back, CH_FULL}};
        auto probe = kos::thread::create_caps(con_nb_probe, nullptr, "connb", 10, caps, 2,
                                              KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                              nb_task);
        int seen = 0;
        if (probe.valid())
        {
            (void)kos_sem_wait(back);
            char const p = 'y';
            g_nb.main_blocking = not con_nonblocking();
            g_nb.main_sent = kos_send_timed(KOS_CAP_STDOUT, &p, 1, CON_PARK_US);
            (void)kos_sem_post(go);
            if (probe.join(CON_JOIN_US) != 0 or kos_task_exit_status(nb_task, &seen) != 0)
            {
                seen = 0;
            }
        }
        g_nb.set = (seen & NB_SET) != 0;
        g_nb.read_back = (seen & NB_READ_BACK) != 0;
        g_nb.sent = -99;
        if ((seen & NB_SENT) != 0)
        {
            g_nb.sent = -KOS_ETIMEDOUT;
        }
        g_nb.tried = 99;
        if ((seen & NB_TRIED) != 0)
        {
            g_nb.tried = 0;
        }
        g_nb.would_block = (seen & NB_WOULD_BLOCK) != 0;
        g_nb.cleared = (seen & NB_CLEARED) != 0;
        (void)kos_task_kill(nb_task);
        (void)kos_handle_close(go);
        (void)kos_handle_close(back);
    }

    void t_console_publish_handout()
    {
        char const probe = '\0';
        if (kos_send_timed(KOS_CAP_STDOUT, &probe, 0, CON_PARK_US) != -KOS_EBADF)
        {
            tap::skip("this image already publishes a console");
            return;
        }
        if (not pool_can_host(4))
        {
            tap::skip("pool too small (a keeper, a writer, a driver and a probe)");
            return;
        }
        kos_cap_t bare = KOS_CAP_NONE;
        TAP_CHECK(kos_endpoint_create(&bare) == 0);
        TAP_CHECK(kos_cap_narrow(bare, KOS_CAP_SIGNAL | KOS_CAP_TRANSFER) == 0);
        int const refused = kos_console_publish(bare, KOS_TASK_NONE);
        int32_t const unseated = kos_send_timed(KOS_CAP_STDOUT, &probe, 0, CON_PARK_US);
        TAP_CHECK(kos_handle_close(bare) == 0);
        TAP_CHECK(refused == -KOS_EACCES);
        TAP_CHECK(unseated == -KOS_EBADF);

        // WAIT without HANDOUT is refused too.
        kos_cap_t waiter = KOS_CAP_NONE;
        TAP_CHECK(kos_endpoint_create(&waiter) == 0);
        TAP_CHECK(kos_cap_narrow(waiter, KOS_CAP_WAIT) == 0);
        int const wait_refused = kos_console_publish(waiter, KOS_TASK_NONE);
        int32_t const wait_unseated = kos_send_timed(KOS_CAP_STDOUT, &probe, 0, CON_PARK_US);
        TAP_CHECK(kos_handle_close(waiter) == 0);
        TAP_CHECK(wait_refused == -KOS_EACCES);
        TAP_CHECK(wait_unseated == -KOS_EBADF);

        kos_task_t drv_task = KOS_TASK_NONE;
        kos_cap_t hold = KOS_CAP_NONE;
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &drv_task) == 0);
        TAP_CHECK(kos_sem_create(0, &hold) == 0);
        kos_cap_grant const hold_caps[] = {{hold, CH_FULL}};
        auto keeper = kos::thread::create_caps(con_keeper, nullptr, "conkeep", 10, hold_caps, 1,
                                               KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                               nullptr, drv_task);
        if (not keeper.valid())
        {
            (void)kos_task_kill(drv_task);
            tap::skip("pool too small");
            return;
        }

        // Narrowed at once, as the init keeps a served endpoint: vacated, HANDOUT alone.
        TAP_CHECK(kos_endpoint_create(&g_con_ep) == 0);
        tap::census_expect(-1); // g_con_ep, which console_publish_narrow closes
        TAP_CHECK(kos_cap_narrow(g_con_ep, CON_KEPT) == 0);
        int const pub = kos_console_publish(g_con_ep, drv_task);
        // main holds WAIT again and the endpoint receives, so the send parks to its deadline.
        int32_t const parked = kos_send_timed(KOS_CAP_STDOUT, &probe, 0, CON_PARK_US);
        int const narrowed = kos_cap_narrow(g_con_ep, CON_KEPT);

        // Nobody receives, and the task lives.
        con_nonblock();
        ConNonblock const nb = g_nb;
        // The writer outranks main, so it has run into its send by the time this returns.
        g_con_sent = -99;
        auto writer = kos::thread::create(con_writer, nullptr, "conwrite", 10);
        int const blocked = writer.join(CON_PARK_US);
        // A receiver comes back and takes the parked line.
        kos_cap_grant caps[] = {{g_con_ep, KOS_CAP_WAIT}};
        auto drv = kos::thread::create_caps(con_driver, nullptr, "condrv", 10, caps, 1,
                                            KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                            drv_task);
        int const drv_joined = drv.join(CON_JOIN_US);
        int const writer_joined = writer.join(CON_JOIN_US);
        // The receiver is gone again and the task lives, so the console is still the driver's.
        int32_t const still = kos_send_timed(KOS_CAP_STDOUT, &probe, 0, 0);
        // The task ends with its entry: the console comes back.
        (void)kos_sem_post(hold);
        int const keeper_joined = keeper.join(CON_JOIN_US);
        if (keeper_joined != 0)
        {
            (void)kos_task_slay(drv_task, CON_SEND_US);
        }
        // main's HANDOUT is left.
        int32_t const after = kos_send(KOS_CAP_STDOUT, &probe, 0);
        (void)kos_task_kill(drv_task);
        (void)kos_handle_close(hold);
        TAP_CHECK(pub == 0);
        TAP_CHECK(parked == -KOS_ETIMEDOUT);
        TAP_CHECK(narrowed == 0);
        TAP_CHECK(nb.tried == 0u);
        TAP_CHECK(nb.sent == -KOS_ETIMEDOUT);
        TAP_CHECK(nb.set);
        TAP_CHECK(nb.read_back);
        TAP_CHECK(nb.would_block);
        TAP_CHECK(nb.cleared);
        TAP_CHECK(nb.main_blocking);
        TAP_CHECK(nb.main_sent == -KOS_ETIMEDOUT);
        TAP_CHECK(nb.fcntl_refusals);
        TAP_CHECK(blocked == -KOS_ETIMEDOUT);
        TAP_CHECK(drv.valid() and drv_joined == 0);
        TAP_CHECK(writer_joined == 0);
        TAP_CHECK(g_con_sent == 2);
        TAP_CHECK(still == -KOS_ETIMEDOUT);
        TAP_CHECK(keeper_joined == 0);
        TAP_CHECK(after == -KOS_EAGAIN);
    }

    // A restart whose start fails before its receiver exists: the task's end alone gives the
    // console back, and main's narrow alone does not.
    void t_console_publish_narrow()
    {
        if (g_con_ep == KOS_CAP_NONE)
        {
            tap::skip("console_publish_handout published nothing");
            return;
        }
        char const probe = '\0';
        kos_task_t drv_task = KOS_TASK_NONE;
        int const created = kos_task_create(nullptr, 0, 0, &drv_task);
        int const pub = kos_console_publish(g_con_ep, drv_task);
        int32_t const parked = kos_send_timed(KOS_CAP_STDOUT, &probe, 0, CON_PARK_US);
        int const narrowed = kos_cap_narrow(g_con_ep, CON_KEPT);
        int32_t const unserved = kos_send_timed(KOS_CAP_STDOUT, &probe, 0, 0);
        int const killed = kos_task_kill(drv_task);
        int32_t const after = kos_send(KOS_CAP_STDOUT, &probe, 0);
        int const closed = kos_handle_close(g_con_ep);
        g_con_ep = KOS_CAP_NONE;
        tap::census_expect(1);
        int32_t const gone = kos_send(KOS_CAP_STDOUT, &probe, 0);
        // Back to the unpublished seat, which every later line falls back from.
        TAP_CHECK(kos_handle_close(KOS_CAP_STDOUT) == 0);
        TAP_CHECK(created == 0);
        TAP_CHECK(pub == 0);
        TAP_CHECK(parked == -KOS_ETIMEDOUT);
        TAP_CHECK(narrowed == 0);
        TAP_CHECK(unserved == -KOS_ETIMEDOUT);
        TAP_CHECK(killed == 0);
        TAP_CHECK(after == -KOS_EAGAIN);
        TAP_CHECK(closed == 0);
        TAP_CHECK(gone == -KOS_ECONNREFUSED);
    }

    // --- shutdown is privileged-only: an unprivileged thread cannot end the system ----
    int g_shutdown_rc = -99;
    void shutdown_denied_worker(void*) // caps: done@1
    {
        // Status 0 on purpose: were the call ever granted, the run ends here with a clean
        // exit status, which the gate sees as a truncated TAP stream.
        g_shutdown_rc = kos_shutdown(0);
        kos_sem_post(CH_DONE);
    }
    void t_shutdown_denied()
    {
        g_shutdown_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto w = kos::thread::create_caps(shutdown_denied_worker, nullptr, "sdden", 10, caps, 1);
        TAP_CHECK(w.valid());
        wait_n(1);
        TAP_CHECK(g_shutdown_rc == -KOS_EPERM);
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // --- reboot-to-bootloader is privileged-only: the refusal arm only -------------
    // On picopi/pizero2350/teensy41 a main kos_reboot really reboots the board mid-run and
    // truncates the TAP stream.
    int g_reboot_rc = -99;
    void reboot_denied_worker(void*) // caps: done@1
    {
        g_reboot_rc = kos_reboot();
        kos_sem_post(CH_DONE);
    }
    void t_reboot_denied()
    {
        g_reboot_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto w = kos::thread::create_caps(reboot_denied_worker, nullptr, "rbden", 10, caps, 1);
        TAP_CHECK(w.valid());
        wait_n(1);
        TAP_CHECK(g_reboot_rc == -KOS_EPERM);
    }
#endif // KICKOS_ENABLE_SELFTEST (reboot refusal)

    // --- a syscall buffer that lives in an app global, from an unprivileged thread ----
    // On a backend that does not model app static data as an MPU region (every no-MPU chip,
    // and the host sim, whose globals sit outside the mprotect'd arena) the raw writable set
    // collapses to the stack alone; user_writable_ok's static-data fallback is what admits a
    // global buffer. recv validates its buffer BEFORE resolving the cap, so a deliberately
    // invalid cap separates the two answers: EBADF means the buffer was admitted, EFAULT
    // means it was not.
    char g_wrbuf[16];
    int32_t g_wrbuf_rc = -99;
    void wrbuf_worker(void*) // caps: done@1
    {
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 0x7fffffff, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        g_wrbuf_rc = kos_reply_recv(KOS_CAP_NONE, g_wrbuf, kos_call_lens_pack(0, sizeof(g_wrbuf)), &opts);
        kos_sem_post(CH_DONE);
    }
    void t_writable_global()
    {
        g_wrbuf_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto w = kos::thread::create_caps(wrbuf_worker, nullptr, "wrGlob", 10, caps, 1);
        TAP_CHECK(w.valid());
        wait_n(1);
        TAP_CHECK(g_wrbuf_rc == -KOS_EBADF); // not -KOS_EFAULT: the global was writable
    }

    // --- the read twin, aimed at app RODATA ------------------------------------------
    // Between them the two arms cover BOTH extents of the app's own window: the writable one
    // above and the read-execute one here. On a translating backend neither is a region any
    // grant path recorded, so what admits either is the granted-range list the address space
    // was seeded with. send validates its buffer before resolving the cap, as recv does. Run
    // from main: a worker is a sibling in main's task holding main's space, so a refusal
    // would present as a failed spawn.
    char const g_rdbuf[16] = "readable-global";
    void t_readable_global()
    {
        TAP_CHECK(kos_send(0x7fffffff, g_rdbuf, sizeof(g_rdbuf)) == -KOS_EBADF);
    }

    // --- The authority capability: the non-privileged arm of the authority gates ------
    // Each authority gate is `privileged OR holds this AUTH_* bit`; main is unprivileged and
    // holds only the bits SELFTEST_AUTHORITY names, so the rest of the suite only exercises the
    // bit-held arm. The child is UNPRIVILEGED and holds AUTH_PINMUX and nothing else, so exactly
    // one gate must accept it and the rest must refuse. Acceptance reads as "not -KOS_EPERM": a
    // gate that lets the call through returns its own answer (-KOS_ENOSYS on a
    // declining-fallback target like the sim, -KOS_EINVAL where a chip owns the block).
    void auth_noop(void*) {}
    Atomic<int32_t, Order::RELAXED> g_auth_pinmux{-99};    // AUTH_PINMUX held    -> anything but -KOS_EPERM
    Atomic<int32_t, Order::RELAXED> g_auth_shutdown{-99};  // AUTH_SYSTEM absent  -> -KOS_EPERM
    Atomic<int32_t, Order::RELAXED> g_auth_regrant{-99};   // may not hand on a bit it does not hold
    Atomic<int32_t, Order::RELAXED> g_auth_toomany{-99};   // cap_count above the spawn-grant bound
    Atomic<int32_t, Order::RELAXED> g_auth_badbits{-99};   // a bit that is no authority at all
    Atomic<int32_t, Order::RELAXED> g_auth_highbit{-99};   // the word's top bit, refused and not truncated
    Atomic<int32_t, Order::RELAXED> g_auth_capsarr{-99};   // the grant ARRAY read, reached past the early refusals
    Atomic<int32_t, Order::RELAXED> g_auth_narrowobj{-99}; // kos_cap_narrow on an object cap narrows its rights
    Atomic<int32_t, Order::RELAXED> g_auth_narrowall{-99}; // a mask of the whole 32-bit word keeps every held bit
    Atomic<int32_t, Order::RELAXED> g_auth_kept{-99};      // so the held bit's gate still answers for itself
    Atomic<int32_t, Order::RELAXED> g_auth_narrowup{-99};  // a mask naming an unheld bit intersects, never grants
    Atomic<int32_t, Order::RELAXED> g_auth_notgained{-99}; // so the gate for that bit still refuses
    Atomic<int32_t, Order::RELAXED> g_auth_narrow{-99};    // giving up the held bit succeeds, needing no authority
    Atomic<int32_t, Order::RELAXED> g_auth_dropped{-99};   // and the gate that accepted it now refuses
    kos_thread_params g_auth_kid;  // deliberately static: see auth_worker
    kos_cap_grant g_auth_two[2];   // ditto
    void auth_worker(void*) // UNPRIVILEGED, authority = AUTH_PINMUX; caps: done@1
    {
        // The bit it HOLDS: past the gate, so pinmux answers for itself.
        g_auth_pinmux = kos_pinmux_set(99u, 0u, 0x10u);
        // A bit it does NOT hold, at a different gate: proves the bits are independent
        // rather than one lump. Safe to call only BECAUSE the child lacks AUTH_SYSTEM: were
        // it granted, the run ends here with a clean status, which the harness sees as a
        // truncated TAP stream.
        g_auth_shutdown = kos_shutdown(0);
        // Three spawn probes off ONE params struct, all refused before a pool slot is
        // claimed, so their codes are deterministic even on a full pool.
        //
        // g_auth_kid and g_auth_two are globals on purpose: thread_create_call reads the params
        // struct and the grant array through user_readable_ok, so a caller may keep either
        // in static data, and that is what this covers.
        kos_thread_params& kid = g_auth_kid;
        kos_thread_t kidh = KOS_THREAD_NONE;
        kid.entry = auth_noop;
        kid.prio = 9;
        // Narrow-only, the same rule a cap_grant mask obeys: holding AUTH_PINMUX does not
        // let it seat AUTH_SYSTEM on a child.
        kid.authority = KOS_AUTH_SYSTEM;
        g_auth_regrant = kos_thread_create(&kid, &kidh);
        // cap_count is bounded by KICKOS_MAX_SPAWN_GRANTS, which is the spawn stager's
        // caller-stack budget and NOT the child table's ceiling. 255 exceeds it on every
        // board. Refused on the COUNT, before the array is read: g_auth_two is two
        // entries long, so a bound checked after the read would fault here.
        g_auth_two[0] = {CH_DONE, CH_FULL};
        g_auth_two[1] = {CH_DONE, CH_FULL};
        kid.caps = g_auth_two;
        kid.cap_count = 255;
        kid.authority = KOS_AUTH_PINMUX;
        g_auth_toomany = kos_thread_create(&kid, &kidh);
        // A bit no gate reads is refused, not masked off. It has to come from ABOVE the
        // eight defined authorities: the authority word has its own numbering, separate
        // from the shared rights byte, so bits 0..7 are all real authorities and an
        // object right like KOS_CAP_WAIT is not a distinguishable wrong value here. The
        // top bit too: a word truncated to a byte on the way in would read it as none.
        kid.cap_count = 1;
        kid.authority = 1u << 8;
        g_auth_badbits = kos_thread_create(&kid, &kidh);
        kid.authority = 1u << 31;
        g_auth_highbit = kos_thread_create(&kid, &kidh);
        // The three probes above are refused before the delegation loop, so none of them
        // reads g_auth_two. Covering the static grant ARRAY needs a probe that gets that
        // far: an unresolvable source_cap is refused -KOS_EBADF from inside the loop,
        // reachable only once the array is admitted. Refused before a slot is claimed.
        g_auth_two[0] = {0x7fffffff, CH_FULL};
        kid.authority = 0;
        g_auth_capsarr = kos_thread_create(&kid, &kidh);
        // An object cap narrows too: the sem cap at CH_DONE keeps SIGNAL, which the post at
        // the end of this worker still needs, and gives up the rest.
        g_auth_narrowobj = kos_cap_narrow(CH_DONE, KOS_CAP_SIGNAL);
        // Every bit of the word in the mask: narrowing by it gives nothing up.
        g_auth_narrowall = kos_cap_narrow(KOS_CAP_AUTHORITY, 0xFFFFFFFFu);
        g_auth_kept = kos_pinmux_set(99u, 0u, 0x10u);
        // A NONZERO mask naming a bit this worker does not hold. The mask is not the new
        // word: narrowing intersects, so asking for AUTH_SYSTEM here must not grant it.
        // A verbatim-seat bug (word = mask) passes a mask-0 test and fails this one.
        g_auth_narrowup = kos_cap_narrow(KOS_CAP_AUTHORITY, KOS_AUTH_PINMUX | KOS_AUTH_SYSTEM);
        g_auth_notgained = kos_shutdown(0); // still refused: AUTH_SYSTEM was never held
        // Needs no authority of its own, and cannot widen: mask 0 gives up everything.
        g_auth_narrow = kos_cap_narrow(KOS_CAP_AUTHORITY, 0);
        // The SAME gate that answered for itself at the top of this worker now refuses.
        g_auth_dropped = kos_pinmux_set(99u, 0u, 0x10u);
        kos_sem_post(CH_DONE);
    }
    void t_authority_cap()
    {
        // main holds exactly its composition's word: the whole of it seats on a child, and
        // the one bit outside it does not.
        TAP_CHECK(g_self->authority == SELFTEST_AUTHORITY);
        auto whole = kos::thread::create_caps(auth_noop, nullptr, "authA", 10, nullptr, 0,
                                              KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                              nullptr, 0, SELFTEST_AUTHORITY);
        TAP_CHECK(whole.valid());
        TAP_CHECK(whole.join() == 0);
        auto outside = kos::thread::create_caps(auth_noop, nullptr, "authP", 10, nullptr, 0,
                                                KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                                nullptr, 0, KOS_AUTH_PSTATE);
        TAP_CHECK(outside.error() == -KOS_EPERM);
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto w = kos::thread::create_caps(auth_worker, nullptr, "authW", 10, caps, 1,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                          nullptr, 0, /*authority=*/KOS_AUTH_PINMUX);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        // Grouped: each TAP_CHECK carries __FILE__ and its stringified condition as
        // rodata.
        int32_t const auth_pinmux = g_auth_pinmux;
        TAP_CHECK(auth_pinmux != -KOS_EPERM and auth_pinmux < 0);
        int32_t const auth_shutdown = g_auth_shutdown;
        TAP_CHECK(auth_shutdown == -KOS_EPERM);
        int32_t const auth_regrant = g_auth_regrant;
        int32_t const auth_toomany = g_auth_toomany;
        int32_t const auth_badbits = g_auth_badbits;
        int32_t const auth_highbit = g_auth_highbit;
        int32_t const auth_capsarr = g_auth_capsarr;
        TAP_CHECK(auth_regrant == -KOS_EPERM and auth_toomany == -KOS_EINVAL
                  and auth_badbits == -KOS_EINVAL and auth_highbit == -KOS_EINVAL
                  and auth_capsarr == -KOS_EBADF);
        int32_t const auth_narrowall = g_auth_narrowall;
        int32_t const auth_kept = g_auth_kept;
        TAP_CHECK(auth_narrowall == 0 and auth_kept != -KOS_EPERM and auth_kept < 0);
        int32_t const auth_narrowobj = g_auth_narrowobj;
        int32_t const auth_narrow = g_auth_narrow;
        int32_t const auth_dropped = g_auth_dropped;
        TAP_CHECK(auth_narrowobj == 0 and auth_narrow == 0
                  and auth_dropped == -KOS_EPERM);
        int32_t const auth_narrowup = g_auth_narrowup;
        int32_t const auth_notgained = g_auth_notgained;
        TAP_CHECK(auth_narrowup == 0 and auth_notgained == -KOS_EPERM);
    }

    // --- Creating a task is an authority ------------------------------------------------
    // A worker holding KOS_AUTH_MEMORY, then its twin holding KOS_AUTH_TASKS as well, each try
    // both ways a task is born and a plain spawn. The tasks bit is the only difference between
    // the two runs, so each refusal is that gate and nothing checked before it. The first run
    // is the M8.5 adversary: an unprivileged thread minting tasks until the pools are empty.
    struct TaskAuthRun
    {
        int32_t create;   // kos_task_create
        int32_t implicit; // a spawn bringing its own data region, which builds a task
        int32_t plain;    // a spawn into the worker's own task
    };
    TaskAuthRun g_ta[2];
    constexpr uint32_t TA_BLOCK = 256;
    constexpr uint32_t TA_JOIN_US = 60000;
    void* g_ta_block = nullptr;
    void ta_child(void*) {}
    int32_t ta_spawn(void* mem, uint32_t mem_size)
    {
        kos_thread_params p{};
        p.entry = ta_child;
        p.name = "taC";
        p.prio = 9;
        p.mem_base = mem;
        p.mem_size = mem_size;
        kos_thread_t h = KOS_THREAD_NONE;
        int32_t const rc = kos_thread_create(&p, &h);
        if (rc == 0)
        {
            (void)kos_thread_join(h, TA_JOIN_US);
        }
        return rc;
    }
    void task_auth_worker(void* arg) // caps: done@1
    {
        TaskAuthRun& out = g_ta[reinterpret_cast<uintptr_t>(arg)];
        kos_task_t t = KOS_TASK_NONE;
        out.create = kos_task_create(nullptr, 0, 0, &t);
        if (out.create == 0)
        {
            (void)kos_task_kill(t);
        }
        out.implicit = ta_spawn(g_ta_block, TA_BLOCK);
        out.plain = ta_spawn(nullptr, 0);
        kos_sem_post(CH_DONE);
    }
    void t_task_authority()
    {
        g_ta_block = kos_ram_alloc(TA_BLOCK);
        if (g_ta_block == nullptr)
        {
            tap::skip("arena cannot spare the data region");
            return;
        }
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        for (uintptr_t run = 0; run < 2; ++run)
        {
            g_ta[run] = {-99, -99, -99};
            uint32_t auth = KOS_AUTH_MEMORY;
            if (run == 1)
            {
                auth |= KOS_AUTH_TASKS;
            }
            auto w = kos::thread::create_caps(task_auth_worker, reinterpret_cast<void*>(run),
                                              "taW", 10, caps, 1, KOS_POLICY_FIFO, 0,
                                              /*privileged=*/false, nullptr, 0, auth);
            if (not w.valid())
            {
                tap::skip("thread pool too small");
                return;
            }
            wait_n(1);
        }
        tap::diag("without the bit: create %d, implicit %d, plain %d; with it: %d, %d, %d",
                  static_cast<int>(g_ta[0].create), static_cast<int>(g_ta[0].implicit),
                  static_cast<int>(g_ta[0].plain), static_cast<int>(g_ta[1].create),
                  static_cast<int>(g_ta[1].implicit), static_cast<int>(g_ta[1].plain));
        TAP_CHECK(g_ta[0].create == -KOS_EPERM and g_ta[0].implicit == -KOS_EPERM
                  and g_ta[0].plain == 0);
        TAP_CHECK(g_ta[1].create == 0 and g_ta[1].implicit == 0 and g_ta[1].plain == 0);
    }

    // --- Peripheral enable: possession is the whole gate ------------------------
    // kos_periph_enable is authorised by holding a live ARCH_MPU_DEV region whose base is
    // EXACTLY the argument, and no authority bit gates it. The possession check runs before
    // the chip backend, so both arms stop in kernel code and never reach silicon, and both
    // run in an UNPRIVILEGED child, a privileged caller bypassing possession. Arm 1 holds no
    // DEV region; arm 2 holds an R|W region whose base matches exactly and must STILL be
    // refused, which pins the ARCH_MPU_DEV attribute filter.
    constexpr uintptr_t PE_BASE = 0x40000000u; // peripheral space, never a code/data/stack base
    Atomic<int32_t, Order::RELAXED> g_pe_unheld{1}; // sentinel: the contract returns 0 or a negative code
    Atomic<int32_t, Order::RELAXED> g_pe_ram{1};
    Atomic<int, Order::RELAXED> g_pe_ram_ran{0};
    void periph_enable_worker(void*) // caps: g_done@1 (CH_DONE)
    {
        g_pe_unheld = kos_periph_enable(PE_BASE);
        // Smallest region this backend can describe, so the arena spend is one block
        // (kos_ram_alloc is a bump allocator with no free).
        void* p = kos_ram_alloc(1);
        if (p != nullptr and kos_mem_self_grant(p, 1, 0) == 0)
        {
            g_pe_ram = kos_periph_enable(reinterpret_cast<uintptr_t>(p));
            g_pe_ram_ran = 1;
        }
        kos_sem_post(CH_DONE);
    }
    void t_periph_enable_unheld()
    {
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto w = kos::thread::create_caps(periph_enable_worker, nullptr, "peW", 10, caps, 1,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                          nullptr, 0, /*authority=*/KOS_AUTH_MEMORY);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        int32_t const pe_unheld = g_pe_unheld;
        TAP_CHECK(pe_unheld == -KOS_EPERM);
        // Arm 2 needs one arch_ram_region_size(1) block, so it runs only on a board whose
        // arena still has one past kmain's two boot stacks and the per-slot default stacks
        // the thread pool bump-allocates.
        int const pe_ram_ran = g_pe_ram_ran;
        TAP_CHECK(pe_ram_ran == 1);
        int32_t const pe_ram = g_pe_ram;
        TAP_CHECK(pe_ram == -KOS_EPERM);
    }

    // --- Privileged register write: the same possession gate, plus the refusal ------
    // Arm 2 finds its DEV window by TRYING the spawn: kos_grant_probe needs
    // KICKOS_ENABLE_SELFTEST and would drop the arm on a production-ABI build; a board that
    // mints no window reports PARTIAL. Arm 3 runs from the UNHELD worker, alignment and wrap
    // being checked before possession. Arm 4 reuses arm 2's window. PRW_OFFSET MUST be
    // 4-aligned or arm 2 stops short of the chip layer, and the BASE keeps it unnameable:
    // the discovery loop steps 0x1000 and the tree's only allowlist is XMC4800's.
    constexpr uintptr_t PRW_OFFSET = 0x4u;
    // Pow2 >= the 32 B PMSA minimum; the discovery step is a multiple of it, so every
    // candidate base stays WIN-aligned (PMSA masks an unaligned base down).
    constexpr uint32_t PRW_WIN = 0x100u;
    // One byte of .bss carries every arm's verdict, not a result word per arm: this file's
    // static RAM is shared by every board, and the tightest margins against the pool-arena
    // link ASSERT are f302nucleo's 608 B and microbit's 768 B. The two workers write it in
    // sequence (each is joined on CH_DONE before the next spawns), so the load/store pair
    // below needs no atomicity.
    constexpr unsigned PRW_UNHELD_OK = 1u << 0;     // arm 1: unheld base refused
    constexpr unsigned PRW_HELD_RAN = 1u << 1;      // arm 2: a window was minted
    constexpr unsigned PRW_HELD_OK = 1u << 2;       // arm 2: declined by the chip layer
    constexpr unsigned PRW_MISALIGNED_OK = 1u << 3; // arm 3: offset not 4-aligned
    constexpr unsigned PRW_WRAP_OK = 1u << 4;       // arm 3: base + offset wraps
    constexpr unsigned PRW_PAST_END_OK = 1u << 5;   // arm 4: first word beyond the window
    constexpr unsigned PRW_FAR_OK = 1u << 6;        // arm 4: 4 KiB beyond the window
    Atomic<unsigned char, Order::RELAXED> g_prw{0};
    void periph_reg_write_held_worker(void* arg) // caps: g_done@1 (CH_DONE)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);
        unsigned seen = PRW_HELD_RAN;
        int32_t const held = kos_periph_reg_write(win, PRW_OFFSET, 0);
        if (held == -KOS_ENOSYS or held == -KOS_EINVAL)
        {
            seen |= PRW_HELD_OK;
        }
        if (kos_periph_reg_write(win, PRW_WIN, 0) == -KOS_EPERM)
        {
            seen |= PRW_PAST_END_OK;
        }
        if (kos_periph_reg_write(win, 0x1000u, 0) == -KOS_EPERM)
        {
            seen |= PRW_FAR_OK;
        }
        g_prw = static_cast<unsigned char>(g_prw | seen);
        kos_sem_post(CH_DONE);
    }
    void periph_reg_write_worker(void*) // caps: g_done@1 (CH_DONE)
    {
        unsigned seen = 0;
        if (kos_periph_reg_write(PE_BASE, PRW_OFFSET, 0) == -KOS_EPERM)
        {
            seen |= PRW_UNHELD_OK;
        }
        // Both malformed-request refusals are checked from an UNHELD caller, which is
        // what makes them discriminate: they run AHEAD of possession, so dropping either
        // check falls through to the possession walk and answers -KOS_EPERM. From a
        // holder the code is -KOS_EINVAL either way (an untabled offset earns the same
        // code), so the arm would be vacuous there.
        if (kos_periph_reg_write(PE_BASE, 0x2u, 0) == -KOS_EINVAL)
        {
            seen |= PRW_MISALIGNED_OK;
        }
        // PE_BASE + ~0x3 wraps at every uintptr_t width (the offset exceeds
        // UINTPTR_MAX - PE_BASE on 32-bit and 64-bit alike), so -KOS_EINVAL is exact
        // here and not width-dependent. The offset is 4-aligned, so the check above
        // cannot answer for this one.
        if (kos_periph_reg_write(PE_BASE, ~static_cast<uintptr_t>(0x3u), 0) == -KOS_EINVAL)
        {
            seen |= PRW_WRAP_OK;
        }
        g_prw = static_cast<unsigned char>(g_prw | seen);
        kos_sem_post(CH_DONE);
    }
    void t_periph_reg_write_unheld()
    {
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto w = kos::thread::create_caps(periph_reg_write_worker, nullptr, "prwW", 10, caps, 1,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                          nullptr, 0, /*authority=*/KOS_AUTH_MEMORY);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        unsigned char const prw = g_prw;
        TAP_CHECK((prw & PRW_UNHELD_OK) != 0);
        // Arm 3: no DEV window needed, so these run on EVERY board including the ones
        // that mint none.
        TAP_CHECK((prw & PRW_MISALIGNED_OK) != 0);
        TAP_CHECK((prw & PRW_WRAP_OK) != 0);

        // Arms 2 and 4 share one window.
        kos::thread::Handle holder;
        for (uintptr_t b = 0x40000000u; b < 0x40100000u; b += 0x1000u)
        {
            holder = kos::thread::create(periph_reg_write_held_worker,
                                         reinterpret_cast<void*>(b), "prwH", 10,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                         nullptr, 0, nullptr, 0,
                                         DeviceWindow(b, PRW_WIN).list(), 1,
                                         caps, 1, KOS_AUTH_MEMORY);
            if (holder.valid())
            {
                break;
            }
            if (holder.error() == -KOS_ENOMEM)
            {
                break; // thread pool, not the window: no later base can succeed either
            }
        }
        if (not holder.valid())
        {
            // The sim admits exactly one DEV window shape (64 KiB, at the base its fake
            // register block landed on), and PRW_WIN is not it, so this discovery loop
            // finds nothing there.
            tap::partial("board mints no free DEV window, so the held arm runs on enforcing "
                         "boards (e.g. the qemu base variant) and, on the host, in "
                         "periph_reg_write_mask");
            return;
        }
        wait_n(1);
        unsigned char const prw_held = g_prw;
        TAP_CHECK((prw_held & PRW_HELD_RAN) != 0);
        TAP_CHECK((prw_held & PRW_HELD_OK) != 0);
        // Arm 4. -KOS_ENOSYS on either of these would mean the offset reached the chip
        // layer, so they discriminate on a board with a backend AND on one without.
        TAP_CHECK((prw_held & PRW_PAST_END_OK) != 0);
        TAP_CHECK((prw_held & PRW_FAR_OK) != 0);
    }

    // --- The allowlist and its value mask, reached on the host ---------------------
#if KICKOS_ARCH_SIM
    // Sim models a privileged-write register block to test allowlist and mask
    // checks. It cannot model the bus privilege rule, so use read-back to check
    // the helper's writes. Other than XMC4800, hardware targets return ENOSYS.
    // Keep candidate addresses, order, and PVS constants equal to sim.cc's
    // SIM_PVREG_BASES, SIM_PVREG_WINDOW, and allowlist.
    constexpr uintptr_t PVS_BASES[] = {
        0x40000000u, 0x100000000ull, 0x400000000ull, 0x10000000000ull, 0x100000000000ull,
    };
    constexpr uint32_t PVS_WIN = 0x10000u;       // the only DEV window shape the sim admits
    constexpr uintptr_t PVS_REG = 0x010u;        // the masked entry, inside the window
    constexpr uintptr_t PVS_UNLISTED = 0x014u;   // inside the window, not on the table
    constexpr uintptr_t PVS_BEYOND = PVS_WIN;    // on the table, OUTSIDE the window
    constexpr uint32_t PVS_MASK = 0x0000C3FFu;   // the entry's whole grant
    constexpr uint32_t PVS_IN = 0x000080FFu;     // a strict subset of it
    // Bit 16 is outside the mask, and the in-mask bits differ from PVS_IN, so a silent
    // TRIM of this value is distinguishable from the refusal by read-back alone.
    constexpr uint32_t PVS_OFF = 0x00010042u;
    constexpr unsigned PVS_RAN = 1u << 0;
    constexpr unsigned PVS_STORE_OK = 1u << 1;      // in-mask value accepted
    constexpr unsigned PVS_STORE_LANDED = 1u << 2;  // and read back exact
    constexpr unsigned PVS_FULLMASK_OK = 1u << 3;   // the whole mask is inside the grant
    constexpr unsigned PVS_OFF_REFUSED = 1u << 4;   // off-mask value -KOS_EINVAL
    constexpr unsigned PVS_OFF_NOTRIM = 1u << 5;    // and the register kept its value
    constexpr unsigned PVS_UNLISTED_OK = 1u << 6;   // untabled offset refused, no store
    constexpr unsigned PVS_BEYOND_OK = 1u << 7;     // tabled but uncontained: -KOS_EPERM
    // Sim-only, so this byte costs no static RAM on a board with an arena floor.
    Atomic<unsigned char, Order::RELAXED> g_pvs{0};
    void pvs_worker(void* arg) // caps: g_done@1 (CH_DONE)
    {
        uintptr_t const PVS_BASE = reinterpret_cast<uintptr_t>(arg);
        unsigned seen = PVS_RAN;
        volatile uint32_t* const reg =
            reinterpret_cast<volatile uint32_t*>(PVS_BASE + PVS_REG);
        volatile uint32_t* const unlisted =
            reinterpret_cast<volatile uint32_t*>(PVS_BASE + PVS_UNLISTED);
        if (kos_periph_reg_write(PVS_BASE, PVS_REG, PVS_IN) == 0)
        {
            seen |= PVS_STORE_OK;
        }
        if (*reg == PVS_IN)
        {
            seen |= PVS_STORE_LANDED;
        }
        // The mask's own value must be admitted: this pins the refusal at the mask EDGE,
        // so a predicate that refuses more than (value & ~mask) cannot pass.
        if (kos_periph_reg_write(PVS_BASE, PVS_REG, PVS_MASK) == 0 and *reg == PVS_MASK)
        {
            seen |= PVS_FULLMASK_OK;
        }
        kos_periph_reg_write(PVS_BASE, PVS_REG, PVS_IN); // known word before the refusal
        if (kos_periph_reg_write(PVS_BASE, PVS_REG, PVS_OFF) == -KOS_EINVAL)
        {
            seen |= PVS_OFF_REFUSED;
        }
        if (*reg == PVS_IN)
        {
            seen |= PVS_OFF_NOTRIM;
        }
        *unlisted = 0;
        if (kos_periph_reg_write(PVS_BASE, PVS_UNLISTED, 0x1u) == -KOS_EINVAL
            and *unlisted == 0)
        {
            seen |= PVS_UNLISTED_OK;
        }
        // Named by the table, one word past the held window: the kernel's containment
        // check is the only thing that refuses it, and -KOS_EPERM (not -KOS_EINVAL)
        // is what proves the refusal came from there and not from the chip layer.
        if (kos_periph_reg_write(PVS_BASE, PVS_BEYOND, 0x1u) == -KOS_EPERM)
        {
            seen |= PVS_BEYOND_OK;
        }
        g_pvs = static_cast<unsigned char>(g_pvs | seen);
        kos_sem_post(CH_DONE);
    }
    void t_periph_reg_write_mask()
    {
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        // Try every candidate, in the sim's own order: exactly one is mapped, and which
        // one depends on the host's address space, not on this test.
        kos::thread::Handle w;
        for (uintptr_t b : PVS_BASES)
        {
            w = kos::thread::create(pvs_worker, reinterpret_cast<void*>(b), "pvsW", 10,
                                    KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0,
                                    nullptr, 0, DeviceWindow(b, PVS_WIN).list(), 1,
                                    caps, 1, KOS_AUTH_MEMORY);
            if (w.valid())
            {
                break;
            }
            if (w.error() == -KOS_ENOMEM)
            {
                break; // thread pool, not the window: no later candidate can succeed
            }
        }
        if (not w.valid())
        {
            // Fails rather than skips: the sim maps the block at one of PVS_BASES, so a
            // refusal points at the grant path, the sim's mapping loop, or a drift between
            // the two lists. A skip would fail the sim gate anyway
            // (FAIL_REGULAR_EXPRESSION "# skipped: [1-9]").
            tap::fail("no candidate DEV window at the sim's fake register block "
                      "(last rc %d): PVS_BASES drifted from SIM_PVREG_BASES, or the "
                      "host owns every candidate", w.error());
            return;
        }
        wait_n(1);
        unsigned char const pvs = g_pvs;
        TAP_CHECK((pvs & PVS_RAN) != 0);
        TAP_CHECK((pvs & PVS_STORE_OK) != 0);
        TAP_CHECK((pvs & PVS_STORE_LANDED) != 0);
        TAP_CHECK((pvs & PVS_FULLMASK_OK) != 0);
        TAP_CHECK((pvs & PVS_OFF_REFUSED) != 0);
        TAP_CHECK((pvs & PVS_OFF_NOTRIM) != 0);
        TAP_CHECK((pvs & PVS_UNLISTED_OK) != 0);
        TAP_CHECK((pvs & PVS_BEYOND_OK) != 0);
    }

    // --- Two device windows in one list, on the host ------------------------------------
    // The sim grants either half of its register span, so one list names both. A list naming
    // a half twice is refused though nothing holds it; the holder of both reaches the seam
    // through each, the chip layer answering the second as the base it does not table; and
    // while it lives the second half alone is refused.
    constexpr unsigned WL_FIRST_OK = 1u << 0;
    constexpr unsigned WL_SECOND_OK = 1u << 1;
    constexpr unsigned WL_ADDR_OK = 1u << 2; // each window answers at its place, nothing past

    Atomic<unsigned char, Order::RELAXED> g_wl{0};
    void wl_holder(void* arg) // caps: done@1, hold@2
    {
        uintptr_t const pv = reinterpret_cast<uintptr_t>(arg);
        unsigned seen = 0;
        if (kos_periph_reg_write(pv, PVS_REG, PVS_IN) == 0)
        {
            seen |= WL_FIRST_OK;
        }
        if (kos_periph_reg_write(pv + PVS_WIN, PVS_REG, PVS_IN) == -KOS_EINVAL)
        {
            seen |= WL_SECOND_OK;
        }
        kos_window first = {};
        kos_window second = {};
        kos_window none = {};
        if (kos_window_get(0, &first) == 0 and first.base == pv and first.size == PVS_WIN
            and kos_window_get(1, &second) == 0 and second.base == pv + PVS_WIN
            and kos_window_get(2, &none) == -KOS_EINVAL)
        {
            seen |= WL_ADDR_OK;
        }
        g_wl = static_cast<unsigned char>(seen);
        kos_sem_wait(CH_DEVHOLD);
        kos_sem_post(CH_DONE);
    }
    // The respawn from a death wake: the server holding the window takes the call and exits
    // holding it, and its teardown wakes the caller, which outranks it and so runs while that
    // teardown is still under way. The window must already be free for the caller's respawn.
    constexpr uint32_t WL_CALL_US = 1000000; // bounds the arm if the server never took the call
    Atomic<int32_t, Order::RELAXED> g_wl_call{-99};
    Atomic<int32_t, Order::RELAXED> g_wl_respawn{-99};
    void wl_noop(void*) {}
    void wl_server(void*) // caps: done@1, E(WAIT)@2
    {
        char b[4];
        struct kos_reply_recv_opts o;
        memset(&o, 0, sizeof(o));
        o.ep = 2;
        o.timeout_us = WL_CALL_US;
        (void)kos_reply_recv(KOS_CAP_NONE, b, kos_call_lens_pack(0, sizeof(b)), &o);
        kos_exit(0); // holding the call, whose caller the teardown answers -KOS_EPIPE
    }
    void wl_caller(void* arg) // caps: done@1, E(SIGNAL)@2
    {
        uintptr_t const pv = reinterpret_cast<uintptr_t>(arg);
        char b[4] = {};
        g_wl_call = kos_call_timed(2, b, sizeof(b), sizeof(b), WL_CALL_US);
        auto const again = kos::thread::create(wl_noop, nullptr, "wlN", 10, KOS_POLICY_FIFO, 0,
                                               false, nullptr, 0, nullptr, 0,
                                               DeviceWindow(pv, PVS_WIN).list(), 1);
        g_wl_respawn = again.error();
        if (again.valid())
        {
            (void)again.join();
        }
        kos_sem_post(CH_DONE);
    }
    void t_window_list()
    {
        kos_cap_t hold = KOS_CAP_NONE;
        if (kos_sem_create(0, &hold) != 0)
        {
            tap::fail("no semaphore for the holder gate");
            return;
        }
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {hold, CH_FULL}};
        g_wl = 0;
        uintptr_t pv = 0;
        int twice = 0;
        for (uintptr_t b : PVS_BASES)
        {
            kos_window const dup[] = {DeviceWindow(b, PVS_WIN).w, DeviceWindow(b, PVS_WIN).w};
            twice = kos::thread::create(wl_holder, nullptr, "wltwice", 10, KOS_POLICY_FIFO, 0,
                                        false, nullptr, 0, nullptr, 0, dup, 2, caps, 2,
                                        KOS_AUTH_MEMORY)
                        .error();
            if (twice != -KOS_EINVAL)
            {
                pv = b; // the mapped candidate: every other one is unencodable
                break;
            }
        }
        TAP_CHECK(twice == -KOS_EBUSY);
        kos_window const both[] = {DeviceWindow(pv, PVS_WIN).w,
                                   DeviceWindow(pv + PVS_WIN, PVS_WIN).w};
        auto const h = kos::thread::create(wl_holder, reinterpret_cast<void*>(pv), "wlboth", 10,
                                           KOS_POLICY_FIFO, 0, false, nullptr, 0, nullptr, 0,
                                           both, 2, caps, 2, KOS_AUTH_MEMORY);
        TAP_CHECK(h.valid());
        if (not h.valid())
        {
            kos_sem_destroy(hold);
            return;
        }
        TAP_CHECK(kos::thread::create(wl_holder, nullptr, "wlsecond", 10, KOS_POLICY_FIFO, 0,
                                      false, nullptr, 0, nullptr, 0,
                                      DeviceWindow(pv + PVS_WIN, PVS_WIN).list(), 1, caps, 2,
                                      KOS_AUTH_MEMORY)
                      .error()
                  == -KOS_EBUSY);
        kos_sem_post(hold);
        wait_n(1);
        (void)h.join(); // its windows are free once it is gone, which the respawn below needs
        kos_sem_destroy(hold);
        unsigned char const wl = g_wl;
        TAP_CHECK((wl & WL_FIRST_OK) != 0);
        TAP_CHECK((wl & WL_SECOND_OK) != 0);
        TAP_CHECK((wl & WL_ADDR_OK) != 0);

        kos_cap_t ep = KOS_CAP_NONE;
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        g_wl_call = -99;
        g_wl_respawn = -99;
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {ep, EP_SIGNAL_ONLY}};
        auto const sv = kos::thread::create(wl_server, nullptr, "wlS", 10, KOS_POLICY_FIFO, 0,
                                            false, nullptr, 0, nullptr, 0,
                                            DeviceWindow(pv, PVS_WIN).list(), 1, scaps, 2,
                                            KOS_AUTH_MEMORY);
        auto const cl = kos::thread::create(wl_caller, reinterpret_cast<void*>(pv), "wlC", 12,
                                            KOS_POLICY_FIFO, 0, false, nullptr, 0, nullptr, 0,
                                            nullptr, 0, ccaps, 2, KOS_AUTH_MEMORY);
        TAP_CHECK(sv.valid() and cl.valid());
        if (cl.valid())
        {
            wait_n(1);
        }
        (void)sv.join();
        (void)kos_handle_close(ep);
        TAP_CHECK(g_wl_call == -KOS_EPIPE);
        TAP_CHECK(g_wl_respawn == 0);
    }
#endif // KICKOS_ARCH_SIM

    // --- No privilege minting after boot ---------------------------------------
    // syscall_thread.cc refuses a privileged child to an unprivileged caller. Runs as
    // main, which is unprivileged, so the refusal costs no thread slot and no arena block
    // (the 16 KiB boards have neither to spare). A posture in which main were privileged
    // turns this red rather than vacuous: the spawn would succeed.
    void escalate_noop(void*) {}
    void t_privileged_spawn_refused()
    {
        TAP_CHECK(kos::thread::create(escalate_noop, nullptr, "escd", 10, KOS_POLICY_FIFO,
                                      0, /*privileged=*/true).error()
                  == -KOS_EPERM);
    }

    // --- Join: the death is observed, and an unreclaimed exit still resolves ----
    // Generous, and only ever spent by a refusal: every join it bounds must answer without
    // parking, so an expiry here is the failure and not the schedule.
    constexpr uint32_t JOIN_GENEROUS_US = 60000;
    // The target holds itself alive across the join below, which is the only thing that
    // separates the PARKED path from the already-EXITED early return: both answer 0, and
    // an instant answer is the signature of the wrong one.
    constexpr uint32_t JOIN_PARK_US = 20000;
    constexpr uint64_t JOIN_PARK_NS = 20000000ull;

    // caps: none. Outlives the join that waits for it, then exits.
    void join_target(void*)
    {
        kos_sleep_ns(JOIN_PARK_NS);
    }

    // caps: none. Exists only to occupy a slot and free it again.
    void join_probe(void*) {}

    void join_stranger(void*) // caps: done@1, lock@2
    {
        // main is the init's child and not this thread's. Issued from a CHILD because main
        // naming itself is the -KOS_EDEADLK case below and would witness nothing about the gate.
        char c = 'x';
        if (kos_thread_join(g_main, JOIN_GENEROUS_US) == -KOS_EPERM)
        {
            c = 'P';
        }
        log_put(c);
        kos_sem_post(CH_DONE);
    }

    void t_thread_join()
    {
        TAP_CHECK(g_main != KOS_THREAD_NONE);
        log_reset();
        // t0 is read BEFORE the spawn: the target outranks main, so it can reach its sleep
        // before the spawn returns, and a t0 taken afterwards would exclude that head start
        // from an interval the check below requires to CONTAIN the sleep.
        uint64_t const t0 = kos_clock_now();
        // One slot at a time: the target is joined before the stranger is spawned.
        auto w = kos::thread::create(join_target, nullptr, "join", 10);
        TAP_CHECK(w.valid());
        // Parked path, and the elapsed time is what says so: a join that answers 0 in less
        // than JOIN_PARK_NS answered from the exited early return instead.
        int const parked_rc = w.join();
        uint32_t const waited_us = static_cast<uint32_t>((kos_clock_now() - t0) / 1000ull);
        TAP_CHECK(parked_rc == 0);
        TAP_CHECK(waited_us >= JOIN_PARK_US); // the target's own exit is what woke it
        // The generation bumps at RECLAIM and not at exit, and main has spawned nothing
        // since, so this handle still names the slot its EXITED occupant holds. The bound
        // is what makes the case total instead of a hang: a kernel that read that state as
        // "still running" answers -KOS_ETIMEDOUT here, and one that read it as a stale
        // handle answers -KOS_EBADF.
        int const exited_rc = kos_thread_join(w.id(), JOIN_GENEROUS_US);
        TAP_CHECK(exited_rc == 0);
        // Refusals that need no target thread: a handle the pool never seats, one whose
        // index is past every slot, and main naming itself.
        TAP_CHECK(kos_thread_join(KOS_THREAD_NONE, JOIN_GENEROUS_US) == -KOS_EBADF);
        TAP_CHECK(kos_thread_join(0x7fffffffu, JOIN_GENEROUS_US) == -KOS_EBADF);
        TAP_CHECK(kos_thread_join(g_main, JOIN_GENEROUS_US) == -KOS_EDEADLK);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto s = kos::thread::create_caps(join_stranger, nullptr, "jstr", 10, caps, 2);
        TAP_CHECK(s.valid());
        wait_n(1);
        TAP_CHECK(log_eq("P")); // parenthood is the whole gate, and nothing delegates it
    }

    // --- Join: a handle whose slot changed hands --------------------------------
    // thread_resolve has two ways to answer nullptr, and t_thread_join's refusals reach only
    // the first (KOS_THREAD_NONE and 0x7fffffff both mask to an index no pool seats). The
    // second is reached by RECLAIM: the pool hands out the LOWEST exited slot, so the second
    // spawn lands on the first target's slot and bumps its generation, leaving the first
    // handle with a seated index and a generation nothing holds.
    // Reclamation is LAZY, at the next spawn, and WHICH slot that spawn takes is not
    // promised: an exited neighbour at a lower index takes it instead, and the stale handle
    // then still names its own seated occupant, which answers 0 rather than -EBADF. So the
    // reseat is established as a FACT from the handle layout (sys/abi.h, 16 index bits under
    // 16 generation bits) before it is asserted on, and a probe that landed elsewhere is
    // PARKED rather than joined, or the allocator hands the same wrong slot back every retry.
    constexpr kos_thread_t JOIN_SLOT_MASK = 0xFFFFu;
    constexpr int JOIN_RESEAT_TRIES = 3;

    void join_holder(void*) // caps: hold@1
    {
        kos_sem_wait(1);
    }

    void t_join_stale_gen()
    {
        if (not pool_can_host(JOIN_RESEAT_TRIES))
        {
            tap::skip("pool too small (%d probes to reseat one slot)", JOIN_RESEAT_TRIES);
            return;
        }
        auto first = kos::thread::create(join_probe, nullptr, "jgn1", 10);
        TAP_CHECK(first.valid());
        TAP_CHECK(first.join() == 0); // EXITED, and its slot is now reclaimable
        kos_thread_t const stale = first.id();
        kos_cap_t hold = KOS_CAP_NONE;
        if (kos_sem_create(0, &hold) != 0)
        {
            tap::skip("no semaphore slot to hold the probes off their slots");
            return;
        }
        kos_cap_grant hcaps[] = {{hold, CH_FULL}};
        kos::thread::Handle probes[JOIN_RESEAT_TRIES];
        int made = 0;
        int reseated = -1;
        for (int i = 0; i < JOIN_RESEAT_TRIES; i++)
        {
            auto w = kos::thread::create_caps(join_holder, nullptr, "jgn2", 10, hcaps, 1);
            if (not w.valid())
            {
                break;
            }
            probes[made] = w;
            made++;
            if ((w.id() & JOIN_SLOT_MASK) == (stale & JOIN_SLOT_MASK))
            {
                reseated = made - 1;
                break;
            }
        }
        // The claim, made only where the premise is a fact: the reseating spawn bumped the
        // generation, so the old handle carries an index the pool DOES seat and a generation
        // nothing holds, which is thread_resolve's second nullptr branch and no other.
        int stale_rc = 0;
        kos_thread_t reseated_id = KOS_THREAD_NONE;
        if (reseated >= 0)
        {
            reseated_id = probes[reseated].id();
            stale_rc = kos_thread_join(stale, JOIN_GENEROUS_US);
        }
        for (int i = 0; i < made; i++)
        {
            kos_sem_post(hold);
        }
        int joined = 0;
        for (int i = 0; i < made; i++)
        {
            joined |= probes[i].join();
        }
        kos_sem_destroy(hold);
        TAP_CHECK(made > 0);
        TAP_CHECK(joined == 0);
        if (reseated < 0)
        {
            // Vacuity, not provisioning: whether a spawn lands on the freed slot depends on
            // what else is live at that instant, so this fires on a loaded box and not on an
            // idle one. A declared skip would have to be present every run to mean anything.
            TAP_SKIP_VACUOUS("%d spawn(s) all landed on slots other than 0x%x, so no handle with "
                             "a seated index and a dead generation was ever constructed",
                             made, static_cast<unsigned>(stale & JOIN_SLOT_MASK));
            return;
        }
        TAP_CHECK(reseated_id != stale); // same index, and the generation moved
        TAP_CHECK(stale_rc == -KOS_EBADF);
    }

    // --- Join: a target that outlives its deadline ------------------------------
    constexpr uint32_t JOIN_TIMEOUT_US = 4000;
    // 20x the deadline, not 1x: at 1x the worker could reach its own exit first and the
    // join would legitimately answer 0.
    constexpr uint64_t JOIN_OUTLIVE_NS = 80000000ull;

    void join_slow(void*) // caps: none
    {
        kos_sleep_ns(JOIN_OUTLIVE_NS);
    }

    void t_join_timeout()
    {
        auto w = kos::thread::create(join_slow, nullptr, "jslo", 10);
        TAP_CHECK(w.valid());
        uint64_t const t0 = kos_clock_now();
        int const rc = w.join(JOIN_TIMEOUT_US);
        uint32_t const waited_us = static_cast<uint32_t>((kos_clock_now() - t0) / 1000ull);
        TAP_CHECK(rc == -KOS_ETIMEDOUT); // the target is still running, and nothing waits on it
        // Both clock reads bracket the syscall, so this cannot pass on a deadline the
        // kernel fired immediately.
        TAP_CHECK(waited_us >= JOIN_TIMEOUT_US);
        // The expiry cleared the wait edge, so the target's exit sweep finds nothing to
        // wake and this is a FRESH park. It must still be woken by that same exit.
        TAP_CHECK(w.join() == 0);
    }

    // --- Tasks: the handle codec, the creator gate, and the group kill ----------
    //
    // A task handle carries a generation over a BIASED index, so the all-zero word is
    // KOS_TASK_NONE and no live task is ever named by it. The gate is CREATORSHIP and takes
    // a second thread to witness, so this arm covers what one thread can see: every refusal
    // the codec produces, and that a hold, once dropped, names nothing.
    void t_task_handles()
    {
        // The out-pointer is validated BEFORE the group exists: a null one is malformed and a
        // misaligned one would take a privileged store the kernel must not make. Checked first,
        // because a mint that cannot deliver its handle leaves a task nothing can name.
        TAP_CHECK(kos_task_create(nullptr, 0, 0, nullptr) == -KOS_EINVAL);
        kos_task_t task = KOS_TASK_NONE;
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &task) == 0);
        TAP_CHECK(task != KOS_TASK_NONE); // the bias is what makes this assertion possible
        // The two words nothing can mint: the sentinel, and a generation the slot never held.
        TAP_CHECK(kos_task_kill(KOS_TASK_NONE) == -KOS_EBADF);
        TAP_CHECK(kos_task_kill(task ^ 0xFFFF0000u) == -KOS_EBADF);
        // An out-of-range index, whatever generation rides it.
        TAP_CHECK(kos_task_kill(0x0000FFFFu) == -KOS_EBADF);
        // An implicit task is unnameable: idle's task is created first and root's second, so
        // slots 0 and 1 hold them and the biased codec names those two handles 1 and 2 at
        // generation 0. Neither slot is ever freed. Idle's carries the KERNEL domain, the
        // whole arena at R|W, so a handle that resolved would hand an unprivileged thread
        // the arena. Root's is reachable to root's own spawns by construction and to nothing
        // else: a plain spawn joins the CALLER's task, never a named one.
        TAP_CHECK(kos_task_kill(1u) == -KOS_EBADF);
        TAP_CHECK(kos_task_kill(2u) == -KOS_EBADF);
        struct kos_thread_params ip = {};
        ip.entry = join_probe;
        ip.name = "timp";
        ip.prio = 10;
        ip.task = 2u; // root's own implicit task
        kos_thread_t ih = KOS_THREAD_NONE;
        TAP_CHECK(kos_thread_create(&ip, &ih) == -KOS_EBADF);
        TAP_CHECK(ih == KOS_THREAD_NONE);

        // A group holding no thread still RESERVES its slot: an implicit task minted by the
        // spawn below must not be handed the slot `task` is sitting in, or that spawn would
        // overwrite the creator tag and `task` would stop naming anything.
        auto probe = kos::thread::create(join_probe, nullptr, "trsv", 10);
        if (probe.valid())
        {
            TAP_CHECK(probe.join() == 0);
        }
        TAP_CHECK(kos_task_kill(task) == 0);
        // The kill DROPPED the hold, and an empty group with no hold is a free slot, so the
        // handle now names nothing. Killing twice is not idempotent, and must not be.
        TAP_CHECK(kos_task_kill(task) == -KOS_EBADF);
    }

    // The creator gate, both halves, and it takes a second thread to witness at all: only the
    // thread that made a group may seat a member into it or end it. Possession of the handle
    // is deliberately not enough, the codec being guessable. The stranger reports over an
    // ENDPOINT and main receives with a DEADLINE: a stranger that never answers must fail
    // this arm rather than hang it.
    void task_stranger(void* arg) // caps: E@1
    {
        kos_task_t const t = static_cast<kos_task_t>(reinterpret_cast<uintptr_t>(arg));
        unsigned char answer[3] = {0u, 0u, 0u};
        struct kos_thread_params p = {};
        p.entry = join_probe;
        p.name = "tsmb";
        p.prio = 10;
        p.task = t;
        kos_thread_t h = KOS_THREAD_NONE;
        if (kos_thread_create(&p, &h) == -KOS_EPERM)
        {
            answer[0] = 1u;
        }
        if (kos_task_kill(t) == -KOS_EPERM)
        {
            answer[1] = 1u;
        }
        // Nor watch it or read its state: both reports are the creator's.
        if (kos_task_watch(t, KOS_CAP_NONE, KOS_CAP_NONE) == -KOS_EPERM
            and kos_task_state(t) == -KOS_EPERM)
        {
            answer[2] = 1u;
        }
        (void)kos_send(KOS_SPAWN_DELEGATED_CAP0, answer, sizeof(answer));
        kos_exit(0);
    }

    void t_task_creator_gate()
    {
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_endpoint_create(&ep) != 0)
        {
            tap::skip("no endpoint slot");
            return;
        }
        kos_task_t task = KOS_TASK_NONE;
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &task) == 0);
        kos_cap_grant const caps[1] = {{ep, CH_FULL}};
        auto stranger = kos::thread::create(
            task_stranger, reinterpret_cast<void*>(static_cast<uintptr_t>(task)), "tstr", 10,
            KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0, nullptr, 0, nullptr, 0,
            caps, 1);
        if (not stranger.valid())
        {
            (void)kos_task_kill(task);
            (void)kos_handle_close(ep);
            tap::skip("pool too small");
            return;
        }
        unsigned char answer[3] = {0u, 0u, 0u};
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, ep, 0, KOS_TIMEOUT_NONE);
        TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, answer, kos_call_lens_pack(0, sizeof(answer)),
                                 &opts)
                  == 3);
        TAP_CHECK(answer[0] == 1u); // a stranger cannot seat a member
        TAP_CHECK(answer[1] == 1u); // nor end the group
        TAP_CHECK(answer[2] == 1u); // nor watch it
        TAP_CHECK(stranger.join() == 0);
        // Still ours, so still killable: the refusals above cost the group nothing.
        TAP_CHECK(kos_task_kill(task) == 0);
        TAP_CHECK(kos_handle_close(ep) == 0);
    }

    // --- The creator's reports: a first receive, and a death ------------------------------
    // A worker holding the task authority is the creator, so the reports go to whoever armed
    // and never to main by name. It runs ABOVE its member: the ready report, raised by the
    // member's first receive before that receive has parked, wakes a thread that outranks the
    // receiver, and the creator must run only once the member is parked, or its own sends and
    // waits below would never return. It arms the watch with a badged copy of a notification
    // it binds, naming one of two endpoints, and closes the copy: the watch names the object,
    // and the binding still receives through it. The member waits on the OTHER endpoint first,
    // which reports nothing, then on the watched one, which reports ready; a second wait there
    // reports nothing more; its exit, the entry's, reports the end and then the death. The
    // state follows each step, and a released task answers -KOS_EBADF.
    constexpr uint32_t TW_BIT = 5;
    constexpr uint32_t TW_QUIET_US = 20000;
    constexpr uint32_t TW_WAIT_US = 500000;
    constexpr uint8_t TW_CREATOR_PRIO = 14;
    constexpr uint8_t TW_MEMBER_PRIO = 11;
    void tw_member(void*) // caps: watched E(WAIT)@1, other E(WAIT)@2
    {
        char b[4];
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, 2, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        (void)kos_reply_recv(KOS_CAP_NONE, b, kos_call_lens_pack(0, sizeof(b)), &o);
        for (int i = 0; i < 2; i++)
        {
            kos_reply_recv_opts_init(&o, 1, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
            (void)kos_reply_recv(KOS_CAP_NONE, b, kos_call_lens_pack(0, sizeof(b)), &o);
        }
        kos_exit(0);
    }
    struct TwRun
    {
        int setup, armed, closed, before, quiet, quiet_state, ready, ready_state, again, ended,
            ended_state, dead, dead_state, killed, stale;
        uint32_t ready_bits, ended_bits;
    };
    TwRun g_tw;
    void tw_play(kos_task_t t, kos_cap_t n, kos_cap_t ep, kos_cap_t other, TwRun* r)
    {
        uint32_t const mask = 1u << TW_BIT;
        uint32_t bits = 0;
        kos_cap_t badged = KOS_CAP_NONE;
        r->armed = kos_notify_badge(n, TW_BIT, &badged);
        if (r->armed == 0)
        {
            r->armed = kos_task_watch(t, badged, ep);
        }
        r->closed = kos_handle_close(badged);
        r->before = kos_task_state(t);
        kos_cap_grant const caps[] = {{ep, KOS_CAP_WAIT}, {other, KOS_CAP_WAIT}};
        auto const m = kos::thread::create_caps(tw_member, nullptr, "twM", TW_MEMBER_PRIO, caps,
                                                2, KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                                nullptr, 0, /*authority=*/0, nullptr, t);
        if (not m.valid())
        {
            return;
        }
        r->quiet = kos_notify_wait(n, mask, TW_QUIET_US, &bits);
        r->quiet_state = kos_task_state(t);
        (void)kos_send(other, "x", 1);
        r->ready = kos_notify_wait(n, mask, TW_WAIT_US, &r->ready_bits);
        r->ready_state = kos_task_state(t);
        (void)kos_send(ep, "x", 1);
        r->again = kos_notify_wait(n, mask, TW_QUIET_US, &bits);
        (void)kos_send(ep, "x", 1);
        r->ended = kos_notify_wait(n, mask, TW_WAIT_US, &r->ended_bits);
        r->ended_state = kos_task_state(t);
        r->dead = 0;
        if (r->ended_state >= 0 and (r->ended_state & KOS_TASK_DEAD) == 0)
        {
            r->dead = kos_notify_wait(n, mask, TW_WAIT_US, &bits);
        }
        r->dead_state = kos_task_state(t);
        r->killed = kos_task_kill(t);
        r->stale = kos_task_state(t);
    }
    void tw_creator(void*) // caps: done@1
    {
        kos_task_t t = KOS_TASK_NONE;
        kos_cap_t n = KOS_CAP_NONE;
        kos_cap_t ep = KOS_CAP_NONE;
        kos_cap_t other = KOS_CAP_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) == 0 and kos_notify_create(&n) == 0
            and kos_endpoint_create(&ep) == 0 and kos_endpoint_create(&other) == 0
            and kos_notify_bind(n) == 0)
        {
            g_tw.setup = 0;
            tw_play(t, n, ep, other, &g_tw);
            (void)kos_notify_unbind(n);
        }
        (void)kos_task_kill(t);
        (void)kos_handle_close(n);
        (void)kos_handle_close(ep);
        (void)kos_handle_close(other);
        kos_sem_post(CH_DONE);
    }
    void t_task_watch_reports()
    {
        g_tw = {-99, -99, -99, -99, -99, -99, -99, -99, -99, -99, -99, -99, -99, -99, -99, 0u,
                0u};
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto w = kos::thread::create_caps(tw_creator, nullptr, "twC", TW_CREATOR_PRIO, caps, 1,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0,
                                          KOS_AUTH_TASKS);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        TwRun const r = g_tw;
        uint32_t const mask = 1u << TW_BIT;
        TAP_CHECK(r.setup == 0 and r.armed == 0 and r.closed == 0 and r.before == 0);
        TAP_CHECK(r.quiet == -KOS_ETIMEDOUT and r.quiet_state == KOS_TASK_LIVE);
        TAP_CHECK(r.ready == 0 and r.ready_bits == mask
                  and r.ready_state == (KOS_TASK_LIVE | KOS_TASK_READY));
        TAP_CHECK(r.again == -KOS_ETIMEDOUT);
        TAP_CHECK(r.ended == 0 and r.ended_bits == mask and r.ended_state >= 0
                  and (r.ended_state & KOS_TASK_ENDED) != 0);
        TAP_CHECK(r.dead == 0 and r.dead_state == (KOS_TASK_DEAD | KOS_TASK_ENDED));
        TAP_CHECK(r.killed == 0 and r.stale == -KOS_EBADF);
    }

    // --- A task ends with its entry, or with a member's fault -----------------------------
    // main creates each task and seats its entry, which spawns the siblings sharing it. The
    // entry's own exit, by returning or through kos_exit, ends the task: its status is set and
    // readable, the watch is raised, every other member is stopped at once, one running no
    // system call included, and the task takes no new member. A sibling's own exit ends only
    // the sibling; a sibling's fault ends the task with the fault's status. The task is dead
    // once every member's teardown is done. Every reading is taken before any check, so a
    // failing check leaves nothing behind for the arms that follow.
    constexpr uint32_t TX_WAIT_US = STALL_TOLERANT_US;
    constexpr uint64_t TX_POLL_NS = 1000000ull;
    // The instant a TX_WAIT_US wait begun now gives up at, which every polling loop below
    // shares instead of counting passes.
    uint64_t tx_give_up()
    {
        return kos_clock_now() + TX_WAIT_US * 1000ull;
    }
    constexpr int TX_ENTRY_EXIT = -3;
    constexpr int TX_SIBLING_EXIT = 7;
    Atomic<int32_t, Order::RELAXED> g_tx_stranger{-99};
    // Bounded: true once kos_task_state(t) carries every bit of `bits`.
    bool tx_await(kos_task_t t, int bits)
    {
        uint64_t const give_up = tx_give_up();
        while (true)
        {
            int const state = kos_task_state(t);
            if (state >= 0 and (state & bits) == bits)
            {
                return true;
            }
            if (kos_clock_now() >= give_up)
            {
                return false;
            }
            kos_sleep_ns(TX_POLL_NS);
        }
    }
    void tx_spins(void*)
    {
        volatile uint32_t turns = 0;
        while (true)
        {
            turns = turns + 1u;
        }
    }
    void tx_noop(void*) {}
    // Spawns a thread into `t` and answers the spawn's code, joining a thread it seated.
    int tx_seat(kos_task_t t)
    {
        auto const h = kos::thread::create(tx_noop, nullptr, "txj", 10, KOS_POLICY_FIFO, 0, false,
                                           nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, 0,
                                           nullptr, t);
        if (h.valid())
        {
            (void)h.join(TX_WAIT_US);
        }
        return h.error();
    }
    void tx_quits(void*)
    {
        kos_exit(TX_SIBLING_EXIT);
    }
    void tx_stranger(void* arg) // caps: done@1
    {
        uintptr_t const t = reinterpret_cast<uintptr_t>(arg);
        int status = 0;
        g_tx_stranger = kos_task_exit_status(static_cast<kos_task_t>(t), &status);
        kos_sem_post(CH_DONE);
    }
    void tx_entry_returns(void*) // caps: go@1
    {
        (void)kos::thread::create(tx_spins, nullptr, "txs", KICKOS_PRIO_MIN);
        kos_sem_wait(1);
    }
    void tx_entry_exits(void*) // caps: go@1, E(SIGNAL)@2
    {
        auto const quits = kos::thread::create(tx_quits, nullptr, "txq", 9);
        if (quits.valid())
        {
            (void)quits.join(TX_WAIT_US);
        }
        (void)kos_send(2, "r", 1);
        kos_sem_wait(1);
        kos_exit(TX_ENTRY_EXIT);
    }
    void t_task_exit_entry_return()
    {
        kos_cap_t go = KOS_CAP_NONE;
        kos_cap_t n = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        if (kos_sem_create(0, &go) != 0 or kos_notify_create(&n) != 0
            or kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            tap::fail("no semaphore, notification or task slot");
            return;
        }
        constexpr uint32_t mask = 1u << TW_BIT;
        kos_cap_t badged = KOS_CAP_NONE;
        int const bound = kos_notify_bind(n);
        int armed = kos_notify_badge(n, TW_BIT, &badged);
        if (armed == 0)
        {
            armed = kos_task_watch(t, badged, KOS_CAP_NONE);
        }
        (void)kos_handle_close(badged);
        kos_cap_grant const caps[] = {{go, KOS_CAP_WAIT}};
        auto const entry = kos::thread::create(tx_entry_returns, nullptr, "txe", 10,
                                               KOS_POLICY_FIFO, 0, false, nullptr, 0, nullptr, 0,
                                               nullptr, 0, caps, 1, 0, nullptr, t);
        int status = -99;
        int const live = kos_task_exit_status(t, &status);
        g_tx_stranger = -99;
        kos_cap_grant const scaps[] = {{g_done, CH_FULL}};
        auto const stranger = kos::thread::create_caps(
            tx_stranger, reinterpret_cast<void*>(static_cast<uintptr_t>(t)), "txx", 10, scaps, 1);
        if (stranger.valid())
        {
            wait_n(1);
            (void)stranger.join();
        }
        kos_sleep_ns(5000000ull);
        kos_sem_post(go);
        // The sibling spins and never enters the kernel: the end stops it all the same.
        uint32_t bits = 0;
        int const ended = kos_notify_wait(n, mask, TX_WAIT_US, &bits);
        int const state = kos_task_state(t);
        bool const dead = tx_await(t, KOS_TASK_DEAD);
        int const later = kos_task_state(t);
        int const got = kos_task_exit_status(t, &status);
        int joined = -1;
        if (entry.valid())
        {
            joined = entry.join(TX_WAIT_US);
        }
        int const refused = tx_seat(t);
        int const slain = kos_task_slay(t, TX_WAIT_US);
        int stale_status = -99;
        int const stale = kos_task_exit_status(t, &stale_status);
        if (bound == 0)
        {
            (void)kos_notify_unbind(n);
        }
        (void)kos_handle_close(n);
        (void)kos_handle_close(go);
        TAP_CHECK(bound == 0 and armed == 0 and entry.valid());
        TAP_CHECK(live == -KOS_EBUSY);
        TAP_CHECK(stranger.valid() and g_tx_stranger == -KOS_EPERM);
        TAP_CHECK(ended == 0 and bits == mask and state >= 0 and (state & KOS_TASK_ENDED) != 0);
        TAP_CHECK(dead and later == (KOS_TASK_DEAD | KOS_TASK_ENDED));
        TAP_CHECK(got == 0 and status == 0 and joined == 0);
        TAP_CHECK(refused == -KOS_EBUSY);
        TAP_CHECK(slain == 0 and stale == -KOS_EBADF and stale_status == -99);
    }
    void t_task_exit_member_exit()
    {
        kos_cap_t go = KOS_CAP_NONE;
        kos_cap_t ready = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        if (kos_sem_create(0, &go) != 0 or kos_endpoint_create(&ready) != 0
            or kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            tap::fail("no semaphore, endpoint or task slot");
            return;
        }
        kos_cap_grant const caps[] = {{go, KOS_CAP_WAIT}, {ready, KOS_CAP_SIGNAL}};
        auto const entry = kos::thread::create(tx_entry_exits, nullptr, "txe", 10,
                                               KOS_POLICY_FIFO, 0, false, nullptr, 0, nullptr, 0,
                                               nullptr, 0, caps, 2, 0, nullptr, t);
        int32_t said = -99;
        if (entry.valid())
        {
            char r = 0;
            struct kos_reply_recv_opts o;
            kos_reply_recv_opts_init(&o, ready, KOS_RECV_NO_INFO, TX_WAIT_US);
            said = kos_reply_recv(KOS_CAP_NONE, &r, kos_call_lens_pack(0, 1), &o);
        }
        // The sibling has exited with its own code, and the task lives on.
        int const state = kos_task_state(t);
        int status = -99;
        int const live = kos_task_exit_status(t, &status);
        kos_sem_post(go);
        bool const dead = tx_await(t, KOS_TASK_DEAD);
        int const final_state = kos_task_state(t);
        int const got = kos_task_exit_status(t, &status);
        int joined = -1;
        if (entry.valid())
        {
            joined = entry.join(TX_WAIT_US);
        }
        int const refused = tx_seat(t);
        int const killed = kos_task_kill(t);
        (void)kos_handle_close(ready);
        (void)kos_handle_close(go);
        TAP_CHECK(entry.valid() and said == 1);
        TAP_CHECK(state == KOS_TASK_LIVE and live == -KOS_EBUSY);
        TAP_CHECK(dead and final_state == (KOS_TASK_DEAD | KOS_TASK_ENDED));
        TAP_CHECK(got == 0 and status == TX_ENTRY_EXIT);
        TAP_CHECK(refused == -KOS_EBUSY);
        TAP_CHECK(joined == 0 and killed == 0);
    }

    // --- A task is dead only once its members' teardown is done --------------------------
    // A creator outranking the entry calls the endpoint the entry alone receives on. The entry
    // fills its table and returns; its teardown closes that receive right first, which ends the
    // call, and sweeps the rest across lock gaps. The creator, running first, reads the task
    // then: ended, no member, not yet dead. All on one core, so the reading is taken there.
    constexpr int TD_LOAD = 8;
    struct TdRun
    {
        int setup, called, mid, dead, final_state;
    };
    TdRun g_td;
    void td_entry(void*)
    {
        for (int i = 0; i < TD_LOAD; i++)
        {
            kos_cap_t s = KOS_CAP_NONE;
            if (kos_sem_create(0, &s) != 0)
            {
                break;
            }
        }
    }
    void td_creator(void*) // caps: done@1
    {
        kos_task_t t = KOS_TASK_NONE;
        kos_cap_t ep = KOS_CAP_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) == 0 and kos_endpoint_create(&ep) == 0)
        {
            kos_cap_grant const caps[] = {{ep, KOS_CAP_WAIT}};
            auto const entry = kos::thread::create(td_entry, nullptr, "tde", 11, KOS_POLICY_FIFO,
                                                   0, false, nullptr, 0, nullptr, 0, nullptr, 0,
                                                   caps, 1, 0, nullptr, t, 1u);
            g_td.setup = entry.error();
            if (entry.valid())
            {
                g_td.setup = kos_cap_narrow(ep, KOS_CAP_SIGNAL);
            }
            if (g_td.setup == 0)
            {
                char b[4] = {};
                g_td.called = kos_call_timed(ep, b, sizeof(b), sizeof(b), TX_WAIT_US);
                g_td.mid = kos_task_state(t);
                g_td.dead = tx_await(t, KOS_TASK_DEAD);
                g_td.final_state = kos_task_state(t);
            }
            if (entry.valid())
            {
                (void)entry.join(TX_WAIT_US);
            }
        }
        (void)kos_task_kill(t);
        (void)kos_handle_close(ep);
        kos_sem_post(CH_DONE);
    }
    void t_task_dead_after_sweep()
    {
        g_td = {-99, -99, -99, 0, -99};
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto const w = kos::thread::create(td_creator, nullptr, "tdc", 12, KOS_POLICY_FIFO, 0,
                                           false, nullptr, 0, nullptr, 0, nullptr, 0, caps, 1,
                                           KOS_AUTH_TASKS, nullptr, KOS_TASK_NONE, 1u);
        if (not w.valid())
        {
            tap::fail("no thread for the creator (rc %d)", w.error());
            return;
        }
        wait_n(1);
        (void)w.join();
        TdRun const r = g_td;
        TAP_CHECK(r.setup == 0);
        TAP_CHECK(r.called < 0);
        TAP_CHECK(r.mid == KOS_TASK_ENDED);
        TAP_CHECK(r.dead == 1 and r.final_state == (KOS_TASK_DEAD | KOS_TASK_ENDED));
    }

    // --- A task is dead only once EVERY member's teardown is done -------------------------
    // Two members with uneven loads: the sibling holds a wide table whose last slot is the only
    // receive right of an endpoint a watcher calls, the entry a single capability. The sibling
    // exits first; its sweep closes the endpoint the entry calls, so the entry, outranking it,
    // runs and exits in the middle of that sweep. The watcher's call ends only when the
    // sibling's sweep reaches its last slot, and the creator, outranking both and woken by the
    // watch, stamps the death: the watcher's stamp has to come first. All on one core.
    struct TeRun
    {
        int setup, entry_call, probe_call, watcher_seq, dead_seq, status_rc, status;
        int pre, entry_rc, sibling_rc, watcher_rc;
    };
    TeRun g_te;
    Atomic<int32_t, Order::RELAXED> g_te_seq{0};
    void te_entry(void*) // caps: E(SIGNAL)@1
    {
        char b[4] = {};
        g_te.entry_call = kos_call_timed(1, b, sizeof(b), sizeof(b), TX_WAIT_US);
    }
    void te_sibling(void*) // caps: E(WAIT)@1, probe E(WAIT) in the last slot
    {
        for (int i = 0; i < TD_LOAD; i++)
        {
            kos_cap_t s = KOS_CAP_NONE;
            if (kos_sem_create(0, &s) != 0)
            {
                break;
            }
        }
    }
    void te_watcher(void*) // caps: probe E(SIGNAL)@1
    {
        char b[4] = {};
        g_te.probe_call = kos_call_timed(1, b, sizeof(b), sizeof(b), TX_WAIT_US);
        g_te_seq = g_te_seq + 1;
        g_te.watcher_seq = g_te_seq;
    }
    void te_creator(void*) // caps: done@1
    {
        kos_task_t t = KOS_TASK_NONE;
        kos_cap_t ep = KOS_CAP_NONE;
        kos_cap_t probe = KOS_CAP_NONE;
        kos_cap_t n = KOS_CAP_NONE;
        kos_cap_t badged = KOS_CAP_NONE;
        int pre = kos_task_create(nullptr, 0, 0, &t);
        if (pre == 0)
        {
            pre = kos_endpoint_create(&ep);
        }
        if (pre == 0)
        {
            pre = kos_endpoint_create(&probe);
        }
        if (pre == 0)
        {
            pre = kos_notify_create(&n);
        }
        if (pre == 0)
        {
            pre = kos_notify_bind(n);
        }
        if (pre == 0)
        {
            pre = kos_notify_badge(n, TW_BIT, &badged);
        }
        if (pre == 0)
        {
            pre = kos_task_watch(t, badged, KOS_CAP_NONE);
        }
        g_te.pre = pre;
        if (pre == 0)
        {
            kos_cap_grant const ecaps[] = {{ep, KOS_CAP_SIGNAL}};
            kos_cap_grant const scaps[] = {{ep, KOS_CAP_WAIT}, {probe, KOS_CAP_WAIT}};
            uint16_t const sdest[] = {0, KICKOS_CAP_CHILD_WIDTH - 1};
            kos_cap_grant const wcaps[] = {{probe, KOS_CAP_SIGNAL}};
            auto const entry = kos::thread::create(te_entry, nullptr, "tee", 12, KOS_POLICY_FIFO,
                                                   0, false, nullptr, 0, nullptr, 0, nullptr, 0,
                                                   ecaps, 1, 0, nullptr, t, 1u);
            auto const sibling = kos::thread::create(te_sibling, nullptr, "tes", 11,
                                                     KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                                     nullptr, 0, nullptr, 0, scaps, 2, 0, sdest,
                                                     t, 1u);
            auto const watcher = kos::thread::create(te_watcher, nullptr, "tew", 13,
                                                     KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                                     nullptr, 0, nullptr, 0, wcaps, 1, 0,
                                                     nullptr, KOS_TASK_NONE, 1u);
            g_te.entry_rc = entry.error();
            g_te.sibling_rc = sibling.error();
            g_te.watcher_rc = watcher.error();
            g_te.setup = entry.error() | sibling.error() | watcher.error();
            if (g_te.setup == 0)
            {
                g_te.setup = kos_cap_narrow(ep, KOS_CAP_SIGNAL)
                             | kos_cap_narrow(probe, KOS_CAP_SIGNAL);
            }
            uint64_t const give_up = tx_give_up();
            while (g_te.setup == 0 and kos_clock_now() < give_up)
            {
                uint32_t bits = 0;
                (void)kos_notify_wait(n, 1u << TW_BIT, TX_WAIT_US, &bits);
                int const state = kos_task_state(t);
                if (state >= 0 and (state & KOS_TASK_DEAD) != 0)
                {
                    g_te_seq = g_te_seq + 1;
                    g_te.dead_seq = g_te_seq;
                    break;
                }
            }
            g_te.status_rc = kos_task_exit_status(t, &g_te.status);
            if (watcher.valid())
            {
                (void)watcher.join(TX_WAIT_US);
            }
            if (entry.valid())
            {
                (void)entry.join(TX_WAIT_US);
            }
            if (sibling.valid())
            {
                (void)sibling.join(TX_WAIT_US);
            }
            (void)kos_notify_unbind(n);
        }
        (void)kos_handle_close(badged);
        (void)kos_handle_close(n);
        (void)kos_task_kill(t);
        (void)kos_handle_close(probe);
        (void)kos_handle_close(ep);
        kos_sem_post(CH_DONE);
    }
    void t_task_dead_after_every_sweep()
    {
        if (not pool_can_host(4))
        {
            tap::skip("pool too small (a creator, an entry, a sibling and a watcher at once)");
            return;
        }
        g_te = {-99, -99, -99, 0, 0, -99, -99, -99, -99, -99, -99};
        g_te_seq = 0;
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto const w = kos::thread::create(te_creator, nullptr, "tec", 14, KOS_POLICY_FIFO, 0,
                                           false, nullptr, 0, nullptr, 0, nullptr, 0, caps, 1,
                                           KOS_AUTH_TASKS, nullptr, KOS_TASK_NONE, 1u);
        if (not w.valid())
        {
            tap::fail("no thread for the creator (rc %d)", w.error());
            return;
        }
        wait_n(1);
        (void)w.join();
        TeRun const r = g_te;
        if (r.setup != 0)
        {
            tap::diag("setup: before the spawns %d, entry %d, sibling %d, watcher %d", r.pre,
                      r.entry_rc, r.sibling_rc, r.watcher_rc);
        }
        TAP_CHECK(r.setup == 0);
        TAP_CHECK(r.entry_call < 0 and r.probe_call == -KOS_ECONNREFUSED);
        TAP_CHECK(r.watcher_seq == 1 and r.dead_seq == 2);
        TAP_CHECK(r.status_rc == 0 and r.status == 0);
    }
#if KICKOS_FAULT_ISOLATION
    // A failing packaged driver thread's trap (kickos::driver::trap): this arch's encoding takes
    // the fault path, which ends the task with the fault's status.
    void tx_driver_traps(void*)
    {
        kickos::driver::trap();
    }
    void t_task_exit_driver_trap()
    {
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            tap::fail("no task slot");
            return;
        }
        auto const entry = kos::thread::create(tx_driver_traps, nullptr, "txtrap", 10,
                                               KOS_POLICY_FIFO, 0, false, nullptr, 0, nullptr, 0,
                                               nullptr, 0, nullptr, 0, 0, nullptr, t);
        bool const dead = tx_await(t, KOS_TASK_DEAD);
        int status = -99;
        int const got = kos_task_exit_status(t, &status);
        int const killed = kos_task_kill(t);
        TAP_CHECK(entry.valid() and dead);
        TAP_CHECK(got == 0 and status == KOS_EXIT_FAULT);
        TAP_CHECK(killed == 0);
    }
#endif
#if KICKOS_MEMORY_ENFORCED && KICKOS_FAULT_ISOLATION
    // Where a space is per task the task's data is its own copy, so a member reports through a
    // block the task shares with main.
    struct TxReport
    {
        void* absent;
        int32_t woke;
        int32_t called;
        int32_t spinning;
    };
#if not KICKOS_HAVE_ASPACE
    TxReport g_tx_report;
#endif
    // The entry and its sibling share one core, where the sibling's lower priority runs it only
    // once the entry has parked.
#if KICKOS_KERNEL_CORES > 1
    constexpr uint32_t TX_ONE_CORE = 1u;
#else
    constexpr uint32_t TX_ONE_CORE = 0u;
#endif
    // A task with its report, main's reservation in `absent`, which no member of the task
    // reaches; null when either is missing.
    TxReport volatile* tx_report_task(kos_task_t* t)
    {
        void* const absent = kos_ram_alloc(64);
#if KICKOS_HAVE_ASPACE
        size_t const g = discover_granule();
        void* const shared = kos_ram_alloc(g);
        if (absent == nullptr or shared == nullptr or kos_mem_self_grant(shared, g, 0) != 0
            or kos_task_create(shared, static_cast<uint32_t>(g), 0, t) != 0)
        {
            return nullptr;
        }
        TxReport volatile* const rep = static_cast<TxReport volatile*>(shared);
#else
        if (absent == nullptr or kos_task_create(nullptr, 0, 0, t) != 0)
        {
            return nullptr;
        }
        TxReport volatile* const rep = &g_tx_report;
#endif
        rep->absent = absent;
        rep->woke = 0;
        rep->called = 0;
        rep->spinning = 0;
        return rep;
    }

    // A sibling's fault, with the entry parked and a second sibling spinning with no system
    // call: the task ends with the fault's status and both are stopped at once, the entry never
    // getting back to its own code.
    void tx_spins_marked(void* arg)
    {
        static_cast<TxReport volatile*>(arg)->spinning = 1;
        tx_spins(nullptr);
    }
    // Faults once the spinning sibling has taken its first turn, or past the bound.
    void tx_touches_once_spinning(void* arg)
    {
        TxReport volatile* const r = static_cast<TxReport volatile*>(arg);
        uint64_t const give_up = tx_give_up();
        while (r->spinning == 0 and kos_clock_now() < give_up)
        {
            kos_sleep_ns(TX_POLL_NS);
        }
        *static_cast<volatile uint32_t*>(r->absent) = 1u;
        kos_exit(TX_SIBLING_EXIT); // unreachable: the store faults
    }
    void tx_entry_waits(void* arg) // caps: go@1
    {
        TxReport volatile* const r = static_cast<TxReport volatile*>(arg);
        (void)kos::thread::create(tx_spins_marked, arg, "txs", KICKOS_PRIO_MIN, KOS_POLICY_FIFO,
                                  0, false, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, 0,
                                  nullptr, KOS_TASK_NONE, TX_ONE_CORE);
        (void)kos::thread::create(tx_touches_once_spinning, arg, "txf", 9, KOS_POLICY_FIFO, 0,
                                  false, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, 0,
                                  nullptr, KOS_TASK_NONE, TX_ONE_CORE);
        kos_sem_wait(1); // nothing posts it, and the sibling's fault slays this thread in it
        r->woke = 1;
        kos_yield();
        r->called = 1;
        kos_exit(TX_ENTRY_EXIT);
    }
    void t_task_exit_member_fault()
    {
        kos_cap_t go = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        TxReport volatile* const rep = tx_report_task(&t);
        if (rep == nullptr or kos_sem_create(0, &go) != 0)
        {
            tap::fail("no reservation, semaphore or task slot");
            return;
        }
        kos_cap_grant const caps[] = {{go, KOS_CAP_WAIT}};
        auto const entry = kos::thread::create(tx_entry_waits,
                                               const_cast<TxReport*>(rep), "txe", 10,
                                               KOS_POLICY_FIFO, 0, false, nullptr, 0, nullptr, 0,
                                               nullptr, 0, caps, 1, 0, nullptr, t, TX_ONE_CORE);
        bool const dead = tx_await(t, KOS_TASK_DEAD);
        int status = -99;
        int const got = kos_task_exit_status(t, &status);
        int joined = -1;
        if (entry.valid())
        {
            joined = entry.join(TX_WAIT_US);
        }
        int const killed = kos_task_kill(t);
        (void)kos_handle_close(go);
        TAP_CHECK(entry.valid() and dead and rep->spinning == 1);
        TAP_CHECK(got == 0 and status == KOS_EXIT_FAULT);
        TAP_CHECK(joined == 0 and killed == 0);
        TAP_CHECK(rep->woke == 0 and rep->called == 0);
    }

    // The entry, killed by main while parked, faults on its way out: the task ends, so it takes
    // no member, but the fault is the canceller's doing and gives it no status. The fault stops
    // the spinning sibling at once, whose first turn tells main the entry has parked.
    void tx_killed_faults(void* arg) // caps: go@1
    {
        TxReport volatile* const r = static_cast<TxReport volatile*>(arg);
        (void)kos::thread::create(tx_spins_marked, arg, "txs", KICKOS_PRIO_MIN, KOS_POLICY_FIFO,
                                  0, false, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, 0,
                                  nullptr, KOS_TASK_NONE, TX_ONE_CORE);
        kos_sem_wait(1); // nothing posts it: the kill ends this wait
        *static_cast<volatile uint32_t*>(r->absent) = 1u;
        kos_exit(TX_ENTRY_EXIT); // unreachable: the store faults
    }
    void t_task_exit_cancelled_fault()
    {
        kos_cap_t go = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        TxReport volatile* const rep = tx_report_task(&t);
        if (rep == nullptr or kos_sem_create(0, &go) != 0)
        {
            tap::fail("no reservation, semaphore or task slot");
            return;
        }
        kos_cap_grant const caps[] = {{go, KOS_CAP_WAIT}};
        auto const entry = kos::thread::create(tx_killed_faults, const_cast<TxReport*>(rep),
                                               "txf", 10, KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                               nullptr, 0, nullptr, 0, caps, 1, 0, nullptr, t,
                                               TX_ONE_CORE);
        uint64_t const give_up = tx_give_up();
        while (rep->spinning == 0 and kos_clock_now() < give_up)
        {
            kos_sleep_ns(TX_POLL_NS);
        }
        int const parked = rep->spinning;
        int const killed = entry.kill();
        int const joined = entry.join(TX_WAIT_US);
        bool const dead = tx_await(t, KOS_TASK_DEAD);
        int const state = kos_task_state(t);
        int status = -99;
        int const got = kos_task_exit_status(t, &status);
        int const refused = tx_seat(t);
        int const slain = kos_task_slay(t, TX_WAIT_US);
        (void)kos_handle_close(go);
        TAP_CHECK(entry.valid() and parked == 1 and killed == 0 and joined == 0);
        TAP_CHECK(dead and state == (KOS_TASK_DEAD | KOS_TASK_ENDED));
        TAP_CHECK(got == 0 and status == KOS_EXIT_CANCELLED);
        TAP_CHECK(refused == -KOS_EBUSY and slain == 0);
    }

    // A task no creator holds, built by a spawn bringing its own memory: nobody can slay it,
    // so its entry's fault slays the sibling spinning with no system call, and the task
    // empties. main sees the last receive right on its endpoint go.
    void tx_implicit_entry(void* arg) // caps: E(WAIT|TRANSFER)@1
    {
        kos_cap_grant const caps[] = {{1, KOS_CAP_WAIT}};
        (void)kos::thread::create(tx_spins, nullptr, "txs", KICKOS_PRIO_MIN, KOS_POLICY_FIFO, 0,
                                  false, nullptr, 0, nullptr, 0, nullptr, 0, caps, 1);
        *static_cast<volatile uint32_t*>(arg) = 1u;
        kos_exit(TX_ENTRY_EXIT); // unreachable: the store faults
    }
    void t_task_exit_implicit_fault()
    {
        size_t const g = discover_granule();
        void* own = nullptr;
        if (g != 0)
        {
            own = kos_ram_alloc(g);
        }
        void* const absent = kos_ram_alloc(64);
        kos_cap_t ep = KOS_CAP_NONE;
        if (own == nullptr or absent == nullptr or kos_endpoint_create(&ep) != 0)
        {
            tap::fail("no reservation or endpoint");
            return;
        }
        kos_cap_grant const caps[] = {{ep, KOS_CAP_WAIT | KOS_CAP_TRANSFER}};
        auto const entry = kos::thread::create(tx_implicit_entry, absent, "txf", 10,
                                               KOS_POLICY_FIFO, 0, false, own,
                                               static_cast<uint32_t>(g), nullptr, 0, nullptr, 0,
                                               caps, 1);
        int const narrowed = kos_cap_narrow(ep, KOS_CAP_SIGNAL);
        int32_t sent = -99;
        uint64_t const give_up = tx_give_up();
        while (entry.valid() and kos_clock_now() < give_up)
        {
            sent = kos_send_timed(ep, "x", 1, 1000u);
            if (sent != -KOS_ETIMEDOUT)
            {
                break;
            }
        }
        int joined = -1;
        if (entry.valid())
        {
            joined = entry.join(TX_WAIT_US);
        }
        (void)kos_handle_close(ep);
        TAP_CHECK(entry.valid() and narrowed == 0 and joined == 0);
        TAP_CHECK(sent == -KOS_ECONNREFUSED);
    }

    // The entry, parked, killed by its spawner and then slain: either death ends the task, so
    // the sibling parked on `go`, which nothing posts, is slain, and the task answers
    // KOS_EXIT_CANCELLED.
    void tx_waits_on_go(void*) // caps: go@1
    {
        kos_sem_wait(1);
        kos_exit(TX_SIBLING_EXIT);
    }
    void tx_entry_parks(void*) // caps: go@1, park@2, E(SIGNAL)@3
    {
        kos_cap_grant const caps[] = {{1, KOS_CAP_WAIT}};
        (void)kos::thread::create(tx_waits_on_go, nullptr, "txw", 9, KOS_POLICY_FIFO, 0, false,
                                  nullptr, 0, nullptr, 0, nullptr, 0, caps, 1);
        (void)kos_send(3, "r", 1);
        kos_sem_wait(2); // nothing posts it: the cancel ends this wait
        kos_exit(TX_ENTRY_EXIT);
    }
    struct TxCancelled
    {
        bool spawned;
        int32_t said;
        int cancelled;
        int joined;
        int state;
        bool dead;
        int got;
        int status;
        int released;
    };
    TxCancelled tx_cancel_entry(bool slay)
    {
        TxCancelled r = {false, -99, -99, -99, -99, false, -99, -99, -99};
        kos_cap_t go = KOS_CAP_NONE;
        kos_cap_t park = KOS_CAP_NONE;
        kos_cap_t ready = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        if (kos_sem_create(0, &go) == 0 and kos_sem_create(0, &park) == 0
            and kos_endpoint_create(&ready) == 0 and kos_task_create(nullptr, 0, 0, &t) == 0)
        {
            kos_cap_grant const caps[] = {{go, CH_FULL}, {park, KOS_CAP_WAIT},
                                          {ready, KOS_CAP_SIGNAL}};
            auto const entry = kos::thread::create(tx_entry_parks, nullptr, "txe", 10,
                                                   KOS_POLICY_FIFO, 0, false, nullptr, 0, nullptr,
                                                   0, nullptr, 0, caps, 3, 0, nullptr, t);
            r.spawned = entry.valid();
            if (r.spawned)
            {
                char c = 0;
                struct kos_reply_recv_opts o;
                kos_reply_recv_opts_init(&o, ready, KOS_RECV_NO_INFO, TX_WAIT_US);
                r.said = kos_reply_recv(KOS_CAP_NONE, &c, kos_call_lens_pack(0, 1), &o);
                if (slay)
                {
                    r.cancelled = entry.slay(TX_WAIT_US);
                }
                else
                {
                    r.cancelled = entry.kill();
                }
                r.joined = entry.join(TX_WAIT_US);
                r.state = kos_task_state(t);
                r.dead = tx_await(t, KOS_TASK_DEAD);
                r.got = kos_task_exit_status(t, &r.status);
            }
        }
        if (t != KOS_TASK_NONE)
        {
            r.released = kos_task_kill(t);
        }
        if (ready != KOS_CAP_NONE)
        {
            (void)kos_handle_close(ready);
        }
        if (park != KOS_CAP_NONE)
        {
            (void)kos_handle_close(park);
        }
        if (go != KOS_CAP_NONE)
        {
            (void)kos_handle_close(go);
        }
        return r;
    }
    void t_task_exit_entry_killed()
    {
        TxCancelled const killed = tx_cancel_entry(false);
        TxCancelled const slain = tx_cancel_entry(true);
        TAP_CHECK(killed.spawned and slain.spawned);
        TAP_CHECK(killed.said == 1 and killed.cancelled == 0 and killed.joined == 0);
        TAP_CHECK((killed.state & KOS_TASK_ENDED) != 0);
        TAP_CHECK(killed.dead and killed.got == 0 and killed.status == KOS_EXIT_CANCELLED);
        TAP_CHECK(killed.released == 0);
        TAP_CHECK(slain.said == 1 and slain.cancelled == 0 and slain.joined == 0);
        TAP_CHECK((slain.state & KOS_TASK_ENDED) != 0);
        TAP_CHECK(slain.dead and slain.got == 0 and slain.status == KOS_EXIT_CANCELLED);
        TAP_CHECK(slain.released == 0);
    }
#endif

    // --- A window asked by its place ------------------------------------------------------
    // Each list holds a device window, on x86 a port window, and a block of main's as a memory
    // window, in more than one order, each list spawned once the one before has exited and
    // given its ranges back. The child asks each window by its place and gets the kind, size and
    // flags it was spawned with: a device at its physical base on a region board and where the
    // kernel mapped it on a translating one, a port window at its first port, the block likewise;
    // nothing past the list. It writes a byte through the block, and a second child holding only
    // the block, read-only, reads that byte at its own place 0.
#if defined(KICKOS_ENABLE_SELFTEST) && (KICKOS_HAVE_MPU || defined(KICKOS_SELFTEST_SPARE_DEV))
    constexpr unsigned char WG_BYTE = 0x3C;
    constexpr int WG_MAX = 3;
    struct WgSeen
    {
        int n;
        int32_t rc[WG_MAX];
        kos_window got[WG_MAX];
        int32_t past;
        int32_t bad_out;
        int32_t read;
        int32_t regrant;
        int32_t retyped;
    };
    WgSeen g_wg;
    // What one of two holders alive at once answered for its place 0.
    struct WgHold
    {
        int32_t rc;
        kos_window got;
    };
    WgHold g_wg_hold[2];
    void wg_child(void*) // caps: done@1
    {
        for (int i = 0; i < g_wg.n; i++)
        {
            g_wg.rc[i] = kos_window_get(static_cast<uint32_t>(i), &g_wg.got[i]);
            if (g_wg.rc[i] == 0 and g_wg.got[i].kind == KOS_WINDOW_MEMORY)
            {
                *reinterpret_cast<volatile unsigned char*>(g_wg.got[i].base) = WG_BYTE;
            }
        }
        kos_window past = {};
        g_wg.past = kos_window_get(static_cast<uint32_t>(g_wg.n), &past);
        g_wg.bad_out = kos_window_get(0, nullptr);
        kos_sem_post(CH_DONE);
    }
    void wg_reader(void*) // caps: done@1
    {
        kos_window w = {};
        if (kos_window_get(0, &w) == 0 and w.kind == KOS_WINDOW_MEMORY
            and w.flags == KOS_WINDOW_RO)
        {
            g_wg.read = *reinterpret_cast<volatile unsigned char*>(w.base);
#if not KICKOS_HAVE_ASPACE && KICKOS_MEMORY_ENFORCED
            // A self-grant of the same block retypes the window's region in place, read-write;
            // the window still answers the flags it was spawned with.
            g_wg.regrant = kos_mem_self_grant(reinterpret_cast<void*>(w.base), w.size, 0);
            kos_window again = {};
            if (kos_window_get(0, &again) == 0)
            {
                g_wg.retyped = again.flags;
            }
#endif
        }
        kos_sem_post(CH_DONE);
    }
    void wg_holder(void* arg) // caps: done@1, hold@2
    {
        WgHold* const h = static_cast<WgHold*>(arg);
        h->rc = kos_window_get(0, &h->got);
        kos_sem_post(CH_DONE);
        kos_sem_wait(2);
    }
    // Spawns a child holding `list`: its handle, which has joined it once valid.
    kos::thread::Handle wg_spawn(void (*entry)(void*), kos_window const* list, int n)
    {
        kos_cap_grant const caps[] = {{g_done, CH_FULL}};
        g_wg.n = n;
        for (int i = 0; i < WG_MAX; i++)
        {
            g_wg.rc[i] = -99;
        }
        g_wg.past = -99;
        g_wg.bad_out = -99;
        g_wg.read = -1;
        g_wg.regrant = -99;
        g_wg.retyped = -99;
        auto const h = kos::thread::create(entry, nullptr, "wgc", 10, KOS_POLICY_FIFO, 0, false,
                                           nullptr, 0, nullptr, 0, list, static_cast<uint16_t>(n),
                                           caps, 1, KOS_AUTH_MEMORY);
        if (h.valid())
        {
            wait_n(1);
            (void)h.join();
        }
        return h;
    }
    // Whether the child answered `list` window by window and nothing past it.
    bool wg_answered(kos_window const* list, int n)
    {
        bool const moved = KICKOS_HAVE_ASPACE;
        bool ok = g_wg.past == -KOS_EINVAL and g_wg.bad_out == -KOS_EINVAL;
        for (int i = 0; i < n; i++)
        {
            kos_window const& got = g_wg.got[i];
            ok = ok and g_wg.rc[i] == 0 and got.kind == list[i].kind
                 and got.size == list[i].size and got.flags == list[i].flags and got.base != 0;
            if (list[i].kind == KOS_WINDOW_PORTS)
            {
                ok = ok and got.base == list[i].base;
            }
            else
            {
                ok = ok and (got.base != list[i].base) == moved;
            }
        }
        return ok;
    }
    // Two holders of a memory window each, the second spawned while the first is parked
    // holding its own: each answers the window it was spawned with.
    bool wg_siblings(kos_window const& first)
    {
        void* const blk = kos_ram_alloc(2u * first.size);
        kos_cap_t hold = KOS_CAP_NONE;
        if (blk == nullptr or kos_sem_create(0, &hold) != 0)
        {
            return false;
        }
        kos_window const second = {reinterpret_cast<uintptr_t>(blk), 2u * first.size,
                                   KOS_WINDOW_MEMORY, 0};
        kos_window const lists[2] = {first, second};
        kos_cap_grant const caps[] = {{g_done, CH_FULL}, {hold, KOS_CAP_WAIT}};
        kos::thread::Handle h[2];
        for (int i = 0; i < 2; i++)
        {
            g_wg_hold[i] = {-99, {}};
            h[i] = kos::thread::create(wg_holder, &g_wg_hold[i], "wgh", 10, KOS_POLICY_FIFO, 0,
                                       false, nullptr, 0, nullptr, 0, &lists[i], 1, caps, 2);
            if (h[i].valid())
            {
                wait_n(1);
            }
        }
        for (int i = 0; i < 2; i++)
        {
            if (h[i].valid())
            {
                kos_sem_post(hold);
            }
        }
        for (int i = 0; i < 2; i++)
        {
            if (h[i].valid())
            {
                (void)h[i].join();
            }
        }
        (void)kos_handle_close(hold);
        bool ok = h[0].valid() and h[1].valid();
        for (int i = 0; i < 2; i++)
        {
            WgHold const& r = g_wg_hold[i];
            ok = ok and r.rc == 0 and r.got.kind == KOS_WINDOW_MEMORY
                 and r.got.size == lists[i].size;
        }
        return ok and g_wg_hold[0].got.base != g_wg_hold[1].got.base;
    }
    void t_window_get()
    {
        size_t const g = discover_granule();
        void* blk = nullptr;
        if (g != 0)
        {
            blk = kos_ram_alloc(g);
        }
        if (blk == nullptr)
        {
            tap::fail("no arena block for the memory window");
            return;
        }
        kos_window const mem = {reinterpret_cast<uintptr_t>(blk), static_cast<uint32_t>(g),
                                KOS_WINDOW_MEMORY, 0};
        // The first device this board admits that nothing holds.
        kos_window dev = {};
        kos::thread::Handle h;
#if defined(KICKOS_SELFTEST_SPARE_DEV)
        dev = {KICKOS_SELFTEST_SPARE_DEV, 0x1000u, KOS_WINDOW_DEVICE, 0};
        kos_window first[] = {dev, mem};
        h = wg_spawn(wg_child, first, 2);
#elif KICKOS_ARCH_SIM
        kos_window first[] = {dev, mem};
        for (uintptr_t b : PVS_BASES)
        {
            first[0] = {b, PVS_WIN, KOS_WINDOW_DEVICE, 0};
            h = wg_spawn(wg_child, first, 2);
            if (h.valid())
            {
                break;
            }
        }
#else
        constexpr uint32_t WIN = 0x100u;
        kos_window first[] = {dev, mem};
        for (uintptr_t b = 0x40000000u; b < 0x40100000u; b += 2u * WIN)
        {
            if (kos_grant_probe(KOS_GRANT_OP_DEV_PRIVILEGED, b, WIN) != 1)
            {
                continue;
            }
            first[0] = {b, WIN, KOS_WINDOW_DEVICE, 0};
            h = wg_spawn(wg_child, first, 2);
            if (h.valid() or h.error() != -KOS_EBUSY)
            {
                break;
            }
        }
#endif
        if (not h.valid())
        {
            tap::fail("no device window admitted (last rc %d)", h.error());
            return;
        }
        dev = first[0];
        bool const first_ok = wg_answered(first, 2);
        kos_window const second[] = {mem, dev};
        bool const second_ok = wg_spawn(wg_child, second, 2).valid() and wg_answered(second, 2);
#if KICKOS_HAVE_ASPACE && defined(__x86_64__)
        kos_window const com2 = {0x2f8u, 8u, KOS_WINDOW_PORTS, 0};
        kos_window const ports_first[] = {com2, mem};
        kos_window const mem_first[] = {mem, com2};
        kos_window const all[] = {dev, com2, mem};
        bool const ports_ok = wg_spawn(wg_child, ports_first, 2).valid()
                              and wg_answered(ports_first, 2)
                              and wg_spawn(wg_child, mem_first, 2).valid()
                              and wg_answered(mem_first, 2)
                              and wg_spawn(wg_child, all, 3).valid() and wg_answered(all, 3);
#else
        bool const ports_ok = true;
#endif
        kos_window const ro = {mem.base, mem.size, KOS_WINDOW_MEMORY, KOS_WINDOW_RO};
        bool const read = wg_spawn(wg_reader, &ro, 1).valid();
        WgSeen const reader = g_wg;
        // Two holders alive at once in main's task, each answered its own place 0.
        bool const siblings_ok = wg_siblings(mem);
        TAP_CHECK(first_ok);
        TAP_CHECK(second_ok);
        TAP_CHECK(ports_ok);
        TAP_CHECK(read and reader.read == WG_BYTE);
#if not KICKOS_HAVE_ASPACE && KICKOS_MEMORY_ENFORCED
        TAP_CHECK(reader.regrant == 0 and reader.retyped == KOS_WINDOW_RO);
#endif
        TAP_CHECK(siblings_ok);
    }
#endif

#if defined(KICKOS_ENABLE_SELFTEST) && not KICKOS_HAVE_ASPACE
    // --- Every reservation handed out zeroed ----------------------------------------------
    // The kernel writes a pattern over arena nothing holds yet, as silicon leaves RAM, and the
    // next reservation lands inside it. A child holding that block through a read-only window
    // reads it whole and finds only zeroes. It cannot witness the data-cache clean that follows
    // the zeroing: no emulated board models a data cache.
    Atomic<int32_t, Order::RELAXED> g_rz_dirty{-1}; // bytes the child found non-zero
    void rz_reader(void*) // caps: done@1
    {
        kos_window w = {};
        if (kos_window_get(0, &w) == 0)
        {
            int32_t dirty = 0;
            for (uint32_t i = 0; i < w.size; i++)
            {
                if (reinterpret_cast<unsigned char const volatile*>(w.base)[i] != 0)
                {
                    dirty++;
                }
            }
            g_rz_dirty = dirty;
        }
        kos_sem_post(CH_DONE);
    }
    void t_ram_alloc_zeroed()
    {
        size_t const g = discover_granule();
        uintptr_t at = 0;
        if (g != 0)
        {
            at = kos_grant_probe(KOS_GRANT_OP_ARENA_SCRIBBLE, 0, 2u * g);
        }
        void* blk = nullptr;
        if (at != 0)
        {
            blk = kos_ram_alloc(g);
        }
        uintptr_t const b = reinterpret_cast<uintptr_t>(blk);
        if (blk == nullptr or b < at or b + g > at + 2u * g)
        {
            tap::fail("the next block is not in the written arena (at 0x%lx, block 0x%lx)",
                      static_cast<unsigned long>(at), static_cast<unsigned long>(b));
            return;
        }
        g_rz_dirty = -1;
        kos_window const ro = {b, static_cast<uint32_t>(g), KOS_WINDOW_MEMORY, KOS_WINDOW_RO};
        kos_cap_grant const caps[] = {{g_done, CH_FULL}};
        auto const r = kos::thread::create(rz_reader, nullptr, "rzr", 10, KOS_POLICY_FIFO, 0,
                                           false, nullptr, 0, nullptr, 0, &ro, 1, caps, 1);
        TAP_CHECK(r.valid());
        if (r.valid())
        {
            wait_n(1);
            (void)r.join();
        }
        TAP_CHECK(g_rz_dirty == 0);
    }

    // The kernel's maintenance of the cache over a block a region holds non-cacheable, counted
    // for each grant that seats one and for IPC into one; a cacheable block costs none.
#if KICKOS_ARCH_ARENA_DCACHE
    constexpr uint64_t UG_PER = 1;
#else
    constexpr uint64_t UG_PER = 0;
#endif
    constexpr uint32_t UG_BLK = 64;
    constexpr uint32_t UG_STACK = 4096;
    constexpr size_t UG_LEN = 16;
    constexpr uint32_t UG_US = 500000;
    // [0] the non-cacheable self-grant, [1] the cacheable one, [2] and [3] the receives into
    // each, [4] the first block's retype to cacheable; all ones where a call failed or a receive
    // delivered the wrong bytes.
    unsigned char* g_ug_blk[2] = {};
    bool g_ug_nocache = false;
    Atomic<uint32_t, Order::RELAXED> g_ug_syncs[5];
    uint64_t ug_syncs()
    {
        return kos_grant_probe(KOS_GRANT_OP_ALIAS_SYNCS, 0, 0);
    }
    // A fresh region set of its own, which main's is not by this point of the run.
    void ug_receiver(void*) // caps: done@1, E(WAIT)@2
    {
        uint32_t flags[2] = {0, 0};
        if (g_ug_nocache)
        {
            flags[0] = KOS_MEM_NOCACHE;
        }
        for (int i = 0; i < 2; i++)
        {
            uint64_t const before = ug_syncs();
            if (kos_mem_self_grant(g_ug_blk[i], UG_BLK, flags[i]) == 0)
            {
                g_ug_syncs[i] = static_cast<uint32_t>(ug_syncs() - before);
            }
        }
        for (int i = 0; i < 2; i++)
        {
            uint64_t const before = ug_syncs();
            struct kos_reply_recv_opts o;
            kos_reply_recv_opts_init(&o, 2, KOS_RECV_NO_INFO, UG_US);
            if (kos_reply_recv(KOS_CAP_NONE, g_ug_blk[i], kos_call_lens_pack(0, UG_LEN), &o)
                    == static_cast<int32_t>(UG_LEN)
                and g_ug_blk[i][UG_LEN - 1] == 0x30u + UG_LEN - 1)
            {
                g_ug_syncs[2 + i] = static_cast<uint32_t>(ug_syncs() - before);
            }
        }
        uint64_t const before = ug_syncs();
        if (kos_mem_self_grant(g_ug_blk[0], UG_BLK, 0) == 0)
        {
            g_ug_syncs[4] = static_cast<uint32_t>(ug_syncs() - before);
        }
        kos_sem_post(CH_DONE);
    }
    void ug_noop(void*) {}
#if KICKOS_MEMORY_ENFORCED
    // A child's cacheable stack over a block a thread holds non-cacheable is refused; once that
    // holder is gone the block is a stack again.
    void* g_ug_stk = nullptr;
    int32_t g_ug_stk_rc = 99;
    void ug_stack_worker(void*) // caps: done@1
    {
        g_ug_stk_rc = -KOS_EINVAL;
        if (kos_mem_self_grant(g_ug_stk, UG_STACK, KOS_MEM_NOCACHE) == 0)
        {
            g_ug_stk_rc = kos::thread::create(ug_noop, nullptr, "ugs", 10, KOS_POLICY_FIFO, 0,
                                              false, nullptr, 0, g_ug_stk, UG_STACK)
                              .error();
        }
        kos_sem_post(CH_DONE);
    }
#endif
    void t_uncached_grant_sync()
    {
        g_ug_blk[0] = static_cast<unsigned char*>(kos_ram_alloc(UG_BLK));
        g_ug_blk[1] = static_cast<unsigned char*>(kos_ram_alloc(UG_BLK));
        void* const win = kos_ram_alloc(UG_BLK);
        void* const hand = kos_ram_alloc(UG_BLK);
        kos_cap_t ep = KOS_CAP_NONE;
        if (g_ug_blk[0] == nullptr or g_ug_blk[1] == nullptr or win == nullptr or hand == nullptr
            or kos_endpoint_create(&ep) != 0)
        {
            tap::skip("no blocks or endpoint left for the arm");
            return;
        }
        TAP_CHECK(static_cast<int64_t>(ug_syncs()) >= 0);
        // Where no non-cacheable region can be seated, the arm asks only that nothing is spent.
        g_ug_nocache = kos_grant_probe(KOS_GRANT_OP_RAM_NOCACHE, reinterpret_cast<uintptr_t>(win),
                                       UG_BLK)
                       == 1;
        uint64_t per = 0;
        if (g_ug_nocache)
        {
            per = UG_PER;
            uint64_t const c0 = ug_syncs();
            kos_window const w = {reinterpret_cast<uintptr_t>(win), UG_BLK, KOS_WINDOW_MEMORY,
                                  KOS_WINDOW_UNCACHED};
            auto const holder = kos::thread::create(ug_noop, nullptr, "ugw", 10, KOS_POLICY_FIFO,
                                                    0, false, nullptr, 0, nullptr, 0, &w, 1,
                                                    nullptr, 0);
            uint64_t const c1 = ug_syncs();
            if (holder.valid())
            {
                (void)holder.join();
            }
            kos_task_t t = KOS_TASK_NONE;
            int const trc = kos_task_create(hand, UG_BLK, KOS_MEM_NOCACHE, &t);
            uint64_t const c2 = ug_syncs();
            if (trc == 0)
            {
                (void)kos_task_kill(t);
            }
            tap::diag("alias syncs: window %lu, task data %lu", static_cast<unsigned long>(c1 - c0),
                      static_cast<unsigned long>(c2 - c1));
            TAP_CHECK(holder.valid() and c1 - c0 == UG_PER);
            TAP_CHECK(trc == 0 and c2 - c1 == UG_PER);
        }
        for (int i = 0; i < 5; i++)
        {
            g_ug_syncs[i] = ~0u;
        }
        unsigned char msg[UG_LEN];
        for (size_t i = 0; i < UG_LEN; i++)
        {
            msg[i] = static_cast<unsigned char>(0x30u + i);
        }
        kos_cap_grant const caps[] = {{g_done, CH_FULL}, {ep, KOS_CAP_WAIT}};
        bool const spawned =
            kos::thread::create_caps(ug_receiver, nullptr, "ugr", TAP_PRIO_PARKS, caps, 2,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, KOS_AUTH_MEMORY,
                                     nullptr, KOS_TASK_NONE, nullptr, 0, TAP_PIN_CORE)
                .valid();
        int32_t sent[2] = {-1, -1};
        if (spawned)
        {
            for (int i = 0; i < 2; i++)
            {
                sent[i] = kos_send_timed(ep, msg, UG_LEN, UG_US);
            }
            wait_n(1);
        }
        (void)kos_handle_close(ep);
        tap::diag("alias syncs: self-grant %lu, cacheable %lu, recv %lu, cacheable recv %lu, "
                  "retype to cacheable %lu",
                  static_cast<unsigned long>(g_ug_syncs[0].load()),
                  static_cast<unsigned long>(g_ug_syncs[1].load()),
                  static_cast<unsigned long>(g_ug_syncs[2].load()),
                  static_cast<unsigned long>(g_ug_syncs[3].load()),
                  static_cast<unsigned long>(g_ug_syncs[4].load()));
        TAP_CHECK(spawned and sent[0] == static_cast<int32_t>(UG_LEN)
                  and sent[1] == static_cast<int32_t>(UG_LEN));
        TAP_CHECK(g_ug_syncs[0].load() == per);
        TAP_CHECK(g_ug_syncs[1].load() == 0);
        TAP_CHECK(g_ug_syncs[2].load() == 2u * per);
        TAP_CHECK(g_ug_syncs[3].load() == 0);
        TAP_CHECK(g_ug_syncs[4].load() == per);
#if KICKOS_MEMORY_ENFORCED
        if (not g_ug_nocache)
        {
            return;
        }
        g_ug_stk = kos_ram_alloc(UG_STACK);
        if (g_ug_stk == nullptr)
        {
            tap::partial("no %lu-byte block left in the arena for the stack refusals",
                         static_cast<unsigned long>(UG_STACK));
            return;
        }
        kos_cap_grant const scaps[] = {{g_done, CH_FULL}};
        auto const worker =
            kos::thread::create_caps(ug_stack_worker, nullptr, "ugk", TAP_PRIO_PARKS, scaps, 1,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, KOS_AUTH_MEMORY);
        if (not worker.valid())
        {
            tap::partial("no thread left for the stack refusals (rc %d)", worker.error());
            return;
        }
        wait_n(1);
        int32_t again = -KOS_EINVAL;
        uint64_t paid = ~0ull;
        bool rejoined = false;
        if (worker.join(UG_US) == 0)
        {
            uint64_t const p0 = ug_syncs();
            auto const child = kos::thread::create(ug_noop, nullptr, "ugs", 10, KOS_POLICY_FIFO, 0,
                                                   false, nullptr, 0, g_ug_stk, UG_STACK);
            paid = ug_syncs() - p0;
            again = child.error();
            if (child.valid())
            {
                rejoined = child.join(UG_US) == 0;
            }
        }
        tap::diag("a stack over a block held non-cacheable: %ld, once its holder is gone: %ld"
                  " paying %lu",
                  static_cast<long>(g_ug_stk_rc), static_cast<long>(again),
                  static_cast<unsigned long>(paid));
        TAP_CHECK(g_ug_stk_rc == -KOS_EBUSY);
        TAP_CHECK(again == 0 and paid == UG_PER and rejoined);
        // The same block again, for a stack inside the spawn's own uncached window and then one
        // over the non-cacheable data of the task it joins: the arena never takes a block back.
        kos_window const uw = {reinterpret_cast<uintptr_t>(g_ug_stk), UG_STACK, KOS_WINDOW_MEMORY,
                               KOS_WINDOW_UNCACHED};
        int32_t const over_window = kos::thread::create(ug_noop, nullptr, "ugv", 10,
                                                        KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                                        g_ug_stk, UG_STACK, &uw, 1)
                                        .error();
        tap::diag("inside its own uncached window: %ld", static_cast<long>(over_window));
        TAP_CHECK(over_window == -KOS_EBUSY);
        kos_task_t nt = KOS_TASK_NONE;
        int const task_rc = kos_task_create(g_ug_stk, UG_STACK, KOS_MEM_NOCACHE, &nt);
        if (task_rc != 0)
        {
            tap::partial("no task to hold the block as its non-cacheable data (rc %d)", task_rc);
            return;
        }
        int32_t const over_data = kos::thread::create(ug_noop, nullptr, "ugd", 10, KOS_POLICY_FIFO,
                                                      0, false, nullptr, 0, g_ug_stk, UG_STACK,
                                                      nullptr, 0, nullptr, 0, 0, nullptr, nt)
                                      .error();
        (void)kos_task_kill(nt);
        tap::diag("over its task's non-cacheable data: %ld", static_cast<long>(over_data));
        TAP_CHECK(over_data == -KOS_EBUSY);
#endif
    }
#endif

    // A member's memory is its TASK's, so a member bringing its own data grant is refused
    // rather than having it silently dropped; and a task nobody created cannot be joined.
    void t_task_member_refusals()
    {
        kos_task_t task = KOS_TASK_NONE;
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &task) == 0);
        void* const blk = kos_ram_alloc(64);
        TAP_CHECK(blk != nullptr);
        struct kos_thread_params p = {};
        p.entry = join_probe;
        p.name = "tmem";
        p.prio = 10;
        p.task = task;
        p.mem_base = blk;
        p.mem_size = 64;
        kos_thread_t h = KOS_THREAD_NONE;
        TAP_CHECK(kos_thread_create(&p, &h) == -KOS_EINVAL);
        TAP_CHECK(h == KOS_THREAD_NONE);
        // Same spawn against a handle no slot answers.
        p.mem_base = nullptr;
        p.mem_size = 0;
        p.task = KOS_TASK_NONE ^ 0x0000FFFFu; // a real generation over no index
        TAP_CHECK(kos_thread_create(&p, &h) == -KOS_EBADF);
        TAP_CHECK(kos_task_kill(task) == 0);
    }

    // The group kill, end to end. One member parks on a semaphore nothing ever posts, the
    // other spins below main and never enters the kernel. The kill stops both at once: the
    // parked one never gets back to its own code, and the JOINs are what prove each died
    // rather than merely being marked.
    Atomic<int, Order::RELAXED> g_task_member_woke{0};
    void task_member(void*) // caps: park@1
    {
        kos_sem_wait(KOS_SPAWN_DELEGATED_CAP0);
        g_task_member_woke = 1;
        kos_exit(1);
    }

    void t_task_group_kill()
    {
        kos_cap_t park = KOS_CAP_NONE;
        if (kos_sem_create(0, &park) != 0)
        {
            tap::skip("no semaphore slot");
            return;
        }
        kos_task_t task = KOS_TASK_NONE;
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &task) == 0);
        kos_cap_grant const caps[1] = {{park, KOS_CAP_WAIT}};
        g_task_member_woke = 0;
        auto member = kos::thread::create(task_member, nullptr, "tmbr", 10, KOS_POLICY_FIFO, 0,
                                          /*privileged=*/false, nullptr, 0, nullptr, 0,
                                          nullptr, 0, caps, 1, /*authority=*/0,
                                          /*cap_dest=*/nullptr, task);
        auto spinner = kos::thread::create(tx_spins, nullptr, "tspn", KICKOS_PRIO_MIN,
                                           KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0,
                                           nullptr, 0, nullptr, 0, nullptr, 0, /*authority=*/0,
                                           /*cap_dest=*/nullptr, task);
        if (not member.valid() or not spinner.valid())
        {
            (void)kos_task_kill(task);
            (void)kos_handle_close(park);
            tap::skip("pool too small");
            return;
        }
        // The spinner takes the core main leaves while it sleeps.
        kos_sleep_ns(2000000ull);
        int const killed = kos_task_kill(task);
        // 0, not ETIMEDOUT: each member has to be GONE, not just marked.
        int const member_joined = member.join(TX_WAIT_US);
        int const spinner_joined = spinner.join(TX_WAIT_US);
        if (spinner_joined != 0)
        {
            (void)spinner.slay();
        }
        int const woke = g_task_member_woke;
        TAP_CHECK(killed == 0);
        TAP_CHECK(spinner_joined == 0);
        TAP_CHECK(member_joined == 0 and woke == 0);
        TAP_CHECK(kos_handle_close(park) == 0);
    }

    // --- A thread raises itself within its task's ceiling -----------------------------------
    // The member starts below a narrowed ceiling: a raise to it is admitted and one past it
    // refused, and so is a priority outside the build's range, the whole argument word read.
    // Each answer it reads wrong sets one bit of the status its task ends with.
    constexpr uint8_t SC_CEILING = 12;
    constexpr uint8_t SC_START = 8;
    void sc_worker(void*)
    {
        int wrong = 0;
        if (kos_thread_set_priority(SC_CEILING) != 0)
        {
            wrong |= 1 << 0;
        }
        if (kos_thread_set_priority(SC_CEILING + 1) != -KOS_EPERM)
        {
            wrong |= 1 << 1;
        }
        if (kos_thread_set_priority(0) != -KOS_EINVAL)
        {
            wrong |= 1 << 2;
        }
        if (kos_thread_set_priority(KICKOS_PRIO_MAX + 1) != -KOS_EINVAL)
        {
            wrong |= 1 << 3;
        }
        // A word whose low byte is in range.
        if (static_cast<int32_t>(arch_syscall(KOS_SYS_THREAD_SET_PRIORITY, 0x100u | SC_START, 0, 0, 0))
            != -KOS_EINVAL)
        {
            wrong |= 1 << 4;
        }
        if (kos_thread_set_priority(KICKOS_PRIO_MIN) != 0)
        {
            wrong |= 1 << 5;
        }
        kos_exit(wrong);
    }

    void t_prio_self_ceiling()
    {
        // main's own ceiling is the one its composition declares.
        int above = -KOS_EPERM;
        if (g_self->ceiling == KICKOS_PRIO_MAX)
        {
            above = -KOS_EINVAL;
        }
        int const at = kos_thread_set_priority(g_self->ceiling);
        int const past = kos_thread_set_priority(static_cast<uint8_t>(g_self->ceiling + 1u));
        int const back = kos_thread_set_priority(g_self->priority);
        TAP_CHECK(at == 0);
        TAP_CHECK(past == above);
        TAP_CHECK(back == 0);
        kos_task_t t = KOS_TASK_NONE;
        if (kos_task_create(nullptr, 0, 0, &t) != 0)
        {
            tap::skip("task pool too small");
            return;
        }
        int const granted = kos_task_sched_grant(t, SC_CEILING, 0);
        auto m = kos::thread::create_caps(sc_worker, nullptr, "scself", SC_START, nullptr, 0,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr, t);
        bool const seated = m.valid();
        int wrong = -1;
        int read = -1;
        if (seated)
        {
            (void)m.join();
            read = kos_task_exit_status(t, &wrong);
        }
        (void)kos_task_kill(t);
        if (not seated)
        {
            tap::skip("pool too small");
            return;
        }
        TAP_CHECK(granted == 0);
        TAP_CHECK(read == 0);
        TAP_CHECK(wrong == 0);
    }

    // --- Slay: the cleanup window a kill leaves and a slay denies ---------------
    // A slain thread's resume is CLAIMED: it runs no further unprivileged instruction and
    // never reaches the window a kill leaves. The window is a plain memory write and it has
    // to be: the death point is the next syscall entry, so a syscall here would make a kill
    // look slain.
    Atomic<int, Order::RELAXED> g_slay_window{0};

    // caps: done@1, park@2. Announces itself, then parks on a semaphore NOTHING ever posts,
    // so the only way past that wait is a cancellation breaking the park.
    void slay_window_worker(void*)
    {
        kos_sem_post(CH_DONE);
        kos_sem_wait(2);
        g_slay_window = g_slay_window + 1;
        kos_exit(0); // never returns: the kernel ends the thread at this syscall's entry
    }

    // The worker outranks main (prio 10 against KICKOS_PRIO_MIN + 1), so its post wakes main
    // without preempting it and main cannot run again until the worker PARKS. Without that,
    // both arms below pass vacuously, the worker having died at the entry to a wait it never
    // reached.
    bool stage_a_parked_slay_worker(kos::thread::Handle* out, kos_cap_t park)
    {
        kos_cap_grant const caps[2] = {{g_done, CH_FULL}, {park, CH_FULL}};
        *out = kos::thread::create_caps(slay_window_worker, nullptr, "slay", 10, caps, 2);
        if (not out->valid())
        {
            return false;
        }
        wait_n(1);
        return true;
    }

    // How many times leg 1 may restage. The worker posts before it parks, and above one
    // kernel core a kill can land in that gap, ending it at its next syscall entry with no
    // window at all. That is a staging that did not take, not a failure of the property.
    constexpr int SLAY_STAGE_TRIES = 8;

    void t_thread_slay_window()
    {
        kos_cap_t park = KOS_CAP_NONE;
        if (kos_sem_create(0, &park) != 0)
        {
            tap::skip("no semaphore slot");
            return;
        }
        // LEG 1, the control. A kill breaks the park and the target returns to userspace for
        // exactly as long as it takes to reach its next syscall.
        int slay_window = 0;
        for (int attempt = 0; attempt < SLAY_STAGE_TRIES and slay_window != 1; attempt++)
        {
            kos::thread::Handle killed;
            if (not stage_a_parked_slay_worker(&killed, park))
            {
                (void)kos_handle_close(park);
                tap::skip("pool too small");
                return;
            }
            g_slay_window = 0;
            int const krc = killed.kill();
            int const jrc = killed.join();
            if (krc != 0 or jrc != 0)
            {
                (void)kos_handle_close(park);
                tap::fail("the control worker could not be killed and joined");
                return;
            }
            slay_window = g_slay_window;
        }
        if (slay_window != 1)
        {
            (void)kos_handle_close(park);
            // A PARTIAL, not a failure. The window is the machine's to grant: every restage
            // lost the race to main's kill, which says nothing about the redirect leg 2
            // judges, so a red here would be a claim about the host and not about the tree.
            // The arm still reddens when the redirect itself is broken, at leg 2.
            tap::partial("the kill window never opened, so the slay leg would be vacuous");
            return;
        }

        // LEG 2, the subject. Same body, same park, same parent: only the verb differs.
        kos::thread::Handle slain;
        if (not stage_a_parked_slay_worker(&slain, park))
        {
            (void)kos_handle_close(park);
            tap::skip("pool too small");
            return;
        }
        // 0, not -KOS_ETIMEDOUT: the call WAITS, and gone is what it returns.
        //
        // The claim holds on either interleaving, which is why this leg needs no restaging.
        // A slay reaching a victim already parked aborts the park and switch_to redirects its
        // resume; one reaching a victim still short of that park is found at the victim's own
        // syscall entry. Neither lets it run a further user instruction, and above one kernel
        // core both are reachable.
        TAP_CHECK(slain.slay() == 0);
        int const slay_window_after = g_slay_window;
        TAP_CHECK(slay_window_after == 1); // unchanged: it executed no further user instruction
        // Gone means gone, and the join is the independent witness of it.
        TAP_CHECK(kos_thread_join(slain.id(), JOIN_GENEROUS_US) == 0);
        TAP_CHECK(kos_handle_close(park) == 0);
    }

    // The gate, which is the kill gate unchanged: slay reaches exactly the set kill reaches.
    void slay_gate_probe(void*) // caps: done@1
    {
        // main is the init's child and not this thread's, so this is the parenthood arm and
        // not the self arm.
        char c = 'x';
        if (kos_thread_slay(g_main, JOIN_GENEROUS_US) == -KOS_EPERM)
        {
            c = 'P';
        }
        log_put(c);
        kos_sem_post(CH_DONE);
    }

    void t_thread_slay_gate()
    {
        log_reset();
        TAP_CHECK(kos_thread_slay(KOS_THREAD_NONE, JOIN_GENEROUS_US) == -KOS_EBADF);
        TAP_CHECK(kos_thread_slay(0x7fffffffu, JOIN_GENEROUS_US) == -KOS_EBADF);
        // Not -KOS_EDEADLK as join answers: ending yourself is kos_exit, and this call has
        // to be able to return to its caller.
        TAP_CHECK(kos_thread_slay(g_main, JOIN_GENEROUS_US) == -KOS_EINVAL);
        // An EXITED-but-unreclaimed slot, which join deliberately ACCEPTS and every cancel
        // deliberately refuses: there is nothing left in it to condemn.
        auto probe = kos::thread::create(join_probe, nullptr, "slgn", 10);
        TAP_CHECK(probe.valid());
        TAP_CHECK(probe.join() == 0);
        TAP_CHECK(kos_thread_slay(probe.id(), JOIN_GENEROUS_US) == -KOS_EBADF);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto s = kos::thread::create_caps(slay_gate_probe, nullptr, "slst", 10, caps, 2);
        TAP_CHECK(s.valid());
        wait_n(1);
        TAP_CHECK(log_eq("P")); // parenthood, and there is no capability to delegate it with
    }

    // --- Slay: the group form ---------------------------------------------------
    // 0 here means a condition no other call in the ABI waits on: the group is EMPTY.
    void t_task_slay_group()
    {
        kos_cap_t park = KOS_CAP_NONE;
        if (kos_sem_create(0, &park) != 0)
        {
            tap::skip("no semaphore slot");
            return;
        }
        kos_task_t task = KOS_TASK_NONE;
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &task) == 0);
        g_slay_window = 0;
        kos_cap_grant const caps[2] = {{g_done, CH_FULL}, {park, CH_FULL}};
        auto member = kos::thread::create(slay_window_worker, nullptr, "tsly", 10,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0,
                                          nullptr, 0, nullptr, 0, caps, 2, /*authority=*/0,
                                          /*cap_dest=*/nullptr, task);
        if (not member.valid())
        {
            (void)kos_task_kill(task);
            (void)kos_handle_close(park);
            tap::skip("pool too small");
            return;
        }
        wait_n(1); // the member outranks main, so it is PARKED once this returns
        TAP_CHECK(kos_task_slay(task, KOS_TIMEOUT_NONE) == 0);
        int const slay_window = g_slay_window;
        TAP_CHECK(slay_window == 0);          // no member got a cleanup window
        TAP_CHECK(member.join(JOIN_GENEROUS_US) == 0); // and the member really is gone
        // The hold went with the wait, so the handle names nothing: a second call cannot
        // resolve it, which is the same shape kos_task_kill leaves behind.
        TAP_CHECK(kos_task_slay(task, JOIN_GENEROUS_US) == -KOS_EBADF);
        TAP_CHECK(kos_handle_close(park) == 0);
    }

    void task_slay_stranger(void* arg) // caps: done@1, lock@2
    {
        kos_task_t const task = static_cast<kos_task_t>(reinterpret_cast<uintptr_t>(arg));
        char c = 'x';
        if (kos_task_slay(task, JOIN_GENEROUS_US) == -KOS_EPERM)
        {
            c = 'P';
        }
        log_put(c);
        kos_sem_post(CH_DONE);
    }

    void t_task_slay_gate()
    {
        log_reset();
        TAP_CHECK(kos_task_slay(KOS_TASK_NONE, JOIN_GENEROUS_US) == -KOS_EBADF);
        kos_task_t task = KOS_TASK_NONE;
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &task) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        auto s = kos::thread::create_caps(task_slay_stranger,
                                          reinterpret_cast<void*>(static_cast<uintptr_t>(task)),
                                          "tsst", 10, caps, 2);
        TAP_CHECK(s.valid());
        wait_n(1);
        TAP_CHECK(log_eq("P")); // creatorship, exactly as kos_task_kill takes it
        // An EMPTY group answers 0 with nothing to slay: dropping the creator's hold is the
        // whole of the work, and a park here would never be woken.
        TAP_CHECK(kos_task_slay(task, KOS_TIMEOUT_NONE) == 0);
        TAP_CHECK(kos_task_slay(task, JOIN_GENEROUS_US) == -KOS_EBADF);
        TAP_CHECK(s.join() == 0);
    }

    // --- A slay returns once EVERY member's teardown is done ---------------------------------
    // The entry returns at once and its end cancels the sibling, which leaves at its first
    // system call. The sibling holds the only receive right of the endpoint the creator calls
    // in its first slot, and of the probe a watcher calls in its last. Its sweep ends the
    // creator's call first, and the creator, outranking it, slays the task in the middle of
    // that sweep, the task empty and still sweeping. The watcher's call ends when the sweep
    // reaches the last slot, so its stamp has to come before the slay's return, and the
    // restart takes the slot back. All on one core.
    struct TsRun
    {
        int setup, called, slain, watcher_seq, slay_seq, restarted, same_slot;
        int pre, entry_rc, sibling_rc, watcher_rc;
    };
    TsRun g_ts;
    Atomic<int32_t, Order::RELAXED> g_ts_seq{0};
    void ts_entry(void*)
    {
    }
    void ts_sibling(void*) // caps: E(WAIT)@0, probe E(WAIT) in the last slot
    {
        kos_yield();
    }
    void ts_watcher(void*) // caps: probe E(SIGNAL)@1
    {
        char b[4] = {};
        (void)kos_call_timed(1, b, sizeof(b), sizeof(b), TX_WAIT_US);
        g_ts_seq = g_ts_seq + 1;
        g_ts.watcher_seq = g_ts_seq;
    }
    void ts_creator(void*) // caps: done@1
    {
        kos_task_t t = KOS_TASK_NONE;
        kos_cap_t ep = KOS_CAP_NONE;
        kos_cap_t probe = KOS_CAP_NONE;
        int pre = kos_task_create(nullptr, 0, 0, &t);
        if (pre == 0)
        {
            pre = kos_endpoint_create(&ep);
        }
        if (pre == 0)
        {
            pre = kos_endpoint_create(&probe);
        }
        g_ts.pre = pre;
        if (pre == 0)
        {
            kos_cap_grant const scaps[] = {{ep, KOS_CAP_WAIT}, {probe, KOS_CAP_WAIT}};
            uint16_t const sdest[] = {0, KICKOS_CAP_CHILD_WIDTH - 1};
            kos_cap_grant const wcaps[] = {{probe, KOS_CAP_SIGNAL}};
            auto const entry = kos::thread::create(ts_entry, nullptr, "tse", 12, KOS_POLICY_FIFO,
                                                   0, false, nullptr, 0, nullptr, 0, nullptr, 0,
                                                   nullptr, 0, 0, nullptr, t, 1u);
            auto const sibling = kos::thread::create(ts_sibling, nullptr, "tss", 11,
                                                     KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                                     nullptr, 0, nullptr, 0, scaps, 2, 0, sdest,
                                                     t, 1u);
            auto const watcher = kos::thread::create(ts_watcher, nullptr, "tsw", 13,
                                                     KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                                     nullptr, 0, nullptr, 0, wcaps, 1, 0,
                                                     nullptr, KOS_TASK_NONE, 1u);
            g_ts.entry_rc = entry.error();
            g_ts.sibling_rc = sibling.error();
            g_ts.watcher_rc = watcher.error();
            g_ts.setup = entry.error() | sibling.error() | watcher.error();
            if (g_ts.setup == 0)
            {
                g_ts.setup = kos_cap_narrow(ep, KOS_CAP_SIGNAL)
                             | kos_cap_narrow(probe, KOS_CAP_SIGNAL);
            }
            if (g_ts.setup == 0)
            {
                char b[4] = {};
                g_ts.called = kos_call_timed(ep, b, sizeof(b), sizeof(b), TX_WAIT_US);
                g_ts.slain = kos_task_slay(t, TX_WAIT_US);
                g_ts_seq = g_ts_seq + 1;
                g_ts.slay_seq = g_ts_seq;
                kos_task_t again = KOS_TASK_NONE;
                g_ts.restarted = kos_task_create(nullptr, 0, 0, &again);
                g_ts.same_slot = ((again ^ t) & 0xFFFFu) == 0u;
                (void)kos_task_kill(again);
            }
            if (watcher.valid())
            {
                (void)watcher.join(TX_WAIT_US);
            }
            if (entry.valid())
            {
                (void)entry.join(TX_WAIT_US);
            }
            if (sibling.valid())
            {
                (void)sibling.join(TX_WAIT_US);
            }
        }
        if (g_ts.slain != 0)
        {
            (void)kos_task_kill(t);
        }
        (void)kos_handle_close(probe);
        (void)kos_handle_close(ep);
        kos_sem_post(CH_DONE);
    }
    void t_task_slay_after_every_sweep()
    {
        if (not pool_can_host(4))
        {
            tap::skip("pool too small (a creator, an entry, a sibling and a watcher at once)");
            return;
        }
        g_ts = {-99, -99, -99, 0, 0, -99, 0, -99, -99, -99, -99};
        g_ts_seq = 0;
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto const w = kos::thread::create(ts_creator, nullptr, "tsc", 14, KOS_POLICY_FIFO, 0,
                                           false, nullptr, 0, nullptr, 0, nullptr, 0, caps, 1,
                                           KOS_AUTH_TASKS, nullptr, KOS_TASK_NONE, 1u);
        if (not w.valid())
        {
            tap::fail("no thread for the creator (rc %d)", w.error());
            return;
        }
        wait_n(1);
        (void)w.join();
        TsRun const r = g_ts;
        if (r.setup != 0)
        {
            tap::diag("setup: before the spawns %d, entry %d, sibling %d, watcher %d", r.pre,
                      r.entry_rc, r.sibling_rc, r.watcher_rc);
        }
        TAP_CHECK(r.setup == 0);
        TAP_CHECK(r.called == -KOS_ECONNREFUSED);
        TAP_CHECK(r.slain == 0);
        TAP_CHECK(r.watcher_seq == 1 and r.slay_seq == 2);
        TAP_CHECK(r.restarted == 0 and r.same_slot == 1);
    }

    // --- Slay: condemned is not yet gone ----------------------------------------
    // The middle guarantee level. It needs a victim that CANNOT be scheduled while the
    // deadline runs, so a higher-priority thread holds the CPU across it: the starvation
    // hazard the timeout exists to make visible instead of hiding in an unbounded park.
    constexpr uint32_t SLAY_TIMEOUT_US = 4000;
    constexpr uint64_t SLAY_HOG_NS = 60000000ull; // 15x the deadline

    // Published so the caller can tell a window it still holds from one it has already
    // spent; 0 until the hog has actually run, which a spawn does not guarantee.
    // The stamp is the low 32 bits of the start, never the 64-bit deadline: a 60 ms window
    // inside the 4.29 s wrap leaves the elapsed arithmetic unambiguous.
    Atomic<uint32_t, Order::RELAXED> g_hog_start_ns{0};

    void slay_hog(void*) // caps: none
    {
        uint64_t const start = kos_clock_now();
        uint64_t const until = start + SLAY_HOG_NS;
        g_hog_start_ns = static_cast<uint32_t>(start);
        while (kos_clock_now() < until)
        {
        }
    }

    // The hog must have more window left than the deadline the caller is about to arm, or
    // the slay measures nothing. Twice the deadline, so the margin is not itself the race.
    bool hog_window_open()
    {
        uint32_t const start = g_hog_start_ns;
        if (start == 0)
        {
            // Not yet run is the healthy case and the opposite of spent: the caller
            // outranks the hog, which first runs when this thread parks inside the slay,
            // so the whole window is still ahead.
            return true;
        }
        uint32_t const window = static_cast<uint32_t>(SLAY_HOG_NS);
        uint32_t const elapsed = static_cast<uint32_t>(kos_clock_now()) - start;
        if (elapsed >= window)
        {
            return false;
        }
        return (window - elapsed) > (2u * SLAY_TIMEOUT_US * 1000u);
    }

    void t_thread_slay_timeout()
    {
        // The instrument is starvation: one hog outranking the victim denies it THE core, so
        // the slay's wait expires while the death stays owed. Denying it EVERY core is not a
        // precondition a test can establish, the placement of N spinners being the
        // scheduler's to choose, so above one core the arm would measure the scheduler.
        TAP_SKIP_ONE_CORE_ORDER();
        kos_cap_t park = KOS_CAP_NONE;
        if (kos_sem_create(0, &park) != 0)
        {
            tap::skip("no semaphore slot");
            return;
        }
        g_slay_window = 0;
        kos::thread::Handle victim;
        if (not stage_a_parked_slay_worker(&victim, park))
        {
            (void)kos_handle_close(park);
            tap::skip("pool too small");
            return;
        }
        // Outranks the victim, so nothing the slay does can get the victim onto the CPU
        // until this thread is finished. Spawned AFTER the victim is staged, because a
        // spawn is not a barrier and this one would otherwise hog the staging itself.
        g_hog_start_ns = 0; // a previous arm's window must not read as this one's
        auto hog = kos::thread::create(slay_hog, nullptr, "shog", 11);
        if (not hog.valid())
        {
            TAP_CHECK(victim.slay() == 0);
            (void)kos_handle_close(park);
            tap::skip("pool too small");
            return;
        }
        // Condemned, not gone: the redirect is armed and irrevocable, and the sweep has not
        // run because the victim has not been given the CPU to run it on. The window runs
        // from the hog's first run and the caller can lose all of it between the two: under
        // an interrupt-driven console the caller blocks there, the hog spends its window and
        // the victim dies at once. slay returning 0 is then correct, so asserting the timeout
        // without this check measures nothing.
        for (int attempt = 0; attempt < 2 and not hog_window_open(); attempt++)
        {
            (void)hog.join();
            g_hog_start_ns = 0;
            hog = kos::thread::create(slay_hog, nullptr, "shog", 11);
            if (not hog.valid())
            {
                break;
            }
        }
        if (not hog.valid() or not hog_window_open())
        {
            if (hog.valid())
            {
                (void)hog.join();
            }
            (void)victim.slay();
            (void)kos_handle_close(park);
            TAP_SKIP_VACUOUS("hog window spent before the slay, starvation not established");
            return;
        }
        TAP_CHECK(victim.slay(SLAY_TIMEOUT_US) == -KOS_ETIMEDOUT);
        int const slay_window = g_slay_window;
        TAP_CHECK(slay_window == 0); // and it never got its window either
        // Irrevocable is the claim, so the same handle must reach GONE with no second
        // request: the timeout gave up on the wait, never on the death.
        TAP_CHECK(kos_thread_join(victim.id(), KOS_TIMEOUT_NONE) == 0);
        int const slay_window_after_join = g_slay_window;
        TAP_CHECK(slay_window_after_join == 0);
        TAP_CHECK(hog.join() == 0);
        TAP_CHECK(kos_handle_close(park) == 0);
    }

    // --- Self-grant, and the region budget that bounds it ----------------------
    // Exercises the REFUSAL at the region-budget ceiling: the call MUST fail loudly
    // (-KOS_ENOMEM) and never truncate the region set. Runs in an unprivileged CHILD: a
    // privileged caller's self-grants are answered "already reachable" without spending a
    // descriptor, so the ceiling would be unreachable. Each descriptor is bought with
    // kos_ram_alloc(1), the smallest region this backend can describe; the blocks are not
    // reclaimed, so the bound below is also what keeps the leak small.
    constexpr int SG_MAX = 12; // > KICKOS_MPU_MAX_REGIONS, so the loop must end refused
    // The ceiling is a different one where a backend translates. No descriptor is seated
    // there, so the self-grant spends no region budget; what runs out is the RESERVATION
    // list, which allocation spends one slot of and never frees, and that is where the
    // -KOS_ENOMEM comes from. The loop therefore asks the space how many slots are
    // left and takes one more than that, so it ends refused whatever the bound is.
    int sg_budget()
    {
#if KICKOS_HAVE_ASPACE
        return static_cast<int>(kos_aspace_probe(KOS_ASPACE_OP_RANGES_FREE, 0)) + 1;
#else
        return SG_MAX;
#endif
    }
    Atomic<int, Order::RELAXED> g_sg_ok{0};       // descriptors accepted before the ceiling
    Atomic<int32_t, Order::RELAXED> g_sg_refusal{0}; // the code that ended the loop
    Atomic<int32_t, Order::RELAXED> g_sg_badsize{0};
    Atomic<int, Order::RELAXED> g_sg_readback{-1};

    void selfgrant_worker(void*)
    {
        // Size-0 refusal costs no arena and no descriptor. The address is a valid
        // stack local, so the refusal is about the SIZE alone.
        int probe = 0;
        g_sg_badsize = kos_mem_self_grant(&probe, 0, 0);
        int const budget = sg_budget();
        for (int i = 0; i < budget; i++)
        {
            void* p = kos_ram_alloc(1);
            if (p == nullptr)
            {
#if KICKOS_HAVE_ASPACE
                // Frames still free is what separates the reservation ceiling from a pool
                // that has actually run out.
                if (kos_aspace_probe(KOS_ASPACE_OP_RANGES_FREE, 0) == 0
                    and kos_aspace_probe(KOS_ASPACE_OP_FRAMES_FREE, 0) != 0)
                {
                    g_sg_refusal = -KOS_ENOMEM;
                    break;
                }
#endif
                g_sg_refusal = 0; // arena, not budget: the parent skips rather than fails
                break;
            }
            int32_t const rc = kos_mem_self_grant(p, 1, 0);
            if (rc != 0)
            {
                g_sg_refusal = rc;
                break;
            }
            // Touch the page just granted: an ungranted write from an unprivileged
            // thread faults, so reaching the readback is the positive half.
            *static_cast<volatile int*>(p) = 0x5A5A + i;
            g_sg_readback = *static_cast<volatile int*>(p) - i;
            g_sg_ok = g_sg_ok + 1;
        }
        kos_sem_post(CH_DONE);
    }
    void t_selfgrant()
    {
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto w = kos::thread::create_caps(selfgrant_worker, nullptr, "sgW", 10, caps, 1,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                          nullptr, 0, /*authority=*/KOS_AUTH_MEMORY);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        if (g_sg_refusal == 0)
        {
            // Vacuity, not provisioning: the ceiling is reached only once the suite's own
            // threads have taken their stacks from the arena, and how much they hold at this
            // instant moves with the box, so the same image skips on one run and not the next.
            TAP_SKIP_VACUOUS("arena too small to reach the region ceiling");
            return;
        }
        // Grouped: each TAP_CHECK carries __FILE__ plus its stringified condition as
        // rodata.
        int const sg_ok = g_sg_ok;
        int32_t const sg_refusal = g_sg_refusal;
        TAP_CHECK(sg_ok > 0 and sg_refusal == -KOS_ENOMEM);
        int const sg_readback = g_sg_readback;
        int32_t const sg_badsize = g_sg_badsize;
        TAP_CHECK(sg_readback == 0x5A5A and sg_badsize == -KOS_EINVAL);
    }

    // Self-grant three-granule blocks on backends that allow non-power-of-two
    // regions. Consecutive bump allocations force a base not aligned to four
    // granules, exposing an invalid size-minus-one alignment check.
    // Run before domain_share to preserve microbit capacity. Bitmap allocators
    // may return aligned bases, so they test the size but not this alignment case.
    Atomic<int32_t, Order::RELAXED> g_sgnp_rc{-1};
    Atomic<int, Order::RELAXED> g_sgnp_ran{0};
    void sgnp_worker(void*)
    {
        size_t const g = discover_granule();
        if (g == 0)
        {
            kos_sem_post(CH_DONE);
            return;
        }
        size_t const want = 3u * g;
        void* pick = nullptr;
        for (int i = 0; i < 3; i++)
        {
            void* p = kos_ram_alloc(want);
            if (p == nullptr)
            {
                break;
            }
            if (pick == nullptr)
            {
                pick = p;
            }
            if ((reinterpret_cast<uintptr_t>(p) & (4u * g - 1u)) != 0)
            {
                pick = p;
                break;
            }
        }
        if (pick != nullptr)
        {
            g_sgnp_rc = kos_mem_self_grant(pick, want, 0);
            g_sgnp_ran = 1;
        }
        kos_sem_post(CH_DONE);
    }
    // --- Which region-encoding mode is live on this board ----------------------
    // The bump allocator's step for a 3-granule request IS the mode: a base+limit backend
    // reserves 3 granules, a pow2 backend rounds to 4. A bump arena only: under translation
    // kos_ram_alloc reserves frames out of a first-fit bitmap, so an earlier free makes two
    // consecutive results non-monotonic and `step` reports pool state. Allocation order is
    // not public API there, so the arm is not registered.
#if not KICKOS_HAVE_ASPACE
    void t_region_mode()
    {
        size_t const g = discover_granule();
        if (g == 0)
        {
            tap::skip("arena too small to discover the granule");
            return;
        }
        void* p = kos_ram_alloc(3u * g);
        void* q = kos_ram_alloc(g);
        if (p == nullptr or q == nullptr)
        {
            tap::skip("arena too small for the mode probe blocks");
            return;
        }
        uintptr_t const a = reinterpret_cast<uintptr_t>(p);
        uintptr_t const b = reinterpret_cast<uintptr_t>(q);
        size_t const step = static_cast<size_t>(b - a);
        // Userspace cannot separate the granular enforcing mode from the no-MPU mode:
        // both allocate granule multiples. The report names the observed shaping only.
        if (step == 3u * g)
        {
            tap::diag("region shaping: GRANULE-MULTIPLE (granule %lu, 3-granule request reserved %lu)",
                      static_cast<unsigned long>(g), static_cast<unsigned long>(step));
        }
        else if (step == 4u * g)
        {
            tap::diag("region shaping: POWER-OF-TWO (granule %lu, 3-granule request reserved %lu)",
                      static_cast<unsigned long>(g), static_cast<unsigned long>(step));
        }
#if defined(KICKOS_MPU_MIN_REGION_CFG) and defined(KICKOS_MPU_REGION_POW2_CFG)
        // Independent oracle: cmake scraped these two literals textually out of the
        // backend .cc this image links, so the expectation shares no source with the
        // arch_mpu_region_pow2() call the allocator made.
        // Undefined on the sim, which never reaches that scrape; the #else branch is
        // the weaker self-consistency check.
        size_t expect = 4u * g;
        if (KICKOS_MPU_MIN_REGION_CFG == 0)
        {
            expect = 3u * g; // no MPU: granule multiples
        }
        else if (KICKOS_MPU_REGION_POW2_CFG == 0)
        {
            expect = 3u * g;
        }
        TAP_CHECK(step == expect);
        if (KICKOS_MPU_MIN_REGION_CFG != 0)
        {
            TAP_CHECK(g == static_cast<size_t>(KICKOS_MPU_MIN_REGION_CFG));
        }
#else
        TAP_CHECK(step == 3u * g or step == 4u * g);
#endif
    }
#endif

    void t_selfgrant_nonpow2()
    {
        // Unprivileged + AUTH_MEMORY, like t_selfgrant: a privileged caller is
        // answered "already reachable" before the geometry is ever examined.
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        auto w = kos::thread::create_caps(sgnp_worker, nullptr, "sgNP", 10, caps, 1,
                                          KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                          nullptr, 0, /*authority=*/KOS_AUTH_MEMORY);
        if (not w.valid())
        {
            tap::skip("thread pool too small");
            return;
        }
        wait_n(1);
        if (g_sgnp_ran == 0)
        {
            tap::skip("arena too small for the probe blocks");
            return;
        }
        int32_t const sgnp_rc = g_sgnp_rc;
        TAP_CHECK(sgnp_rc == 0);
    }
}

namespace
{
    // A failing arm can return with the shared g_ep still open.
    void after_failure()
    {
        (void)kos_handle_close(g_ep);
        g_ep = KOS_CAP_NONE;
        done_reset();
    }
}

extern "C" void selftest_main(kos_self_t const* self)
{
    g_self = self;
    g_main = kos_thread_self();
    kos_sem_create(1, &g_lock);
    kos_sem_create(0, &g_done);
    tap::set_after_failure(after_failure);
    tap::set_census(cap_census, "main's free capability slots");
#if KICKOS_HAVE_ASPACE && defined(KICKOS_ENABLE_SELFTEST)
    g_live_rest = kos_aspace_probe(KOS_ASPACE_OP_THREADS_LIVE, 0);
#endif

// Region 1: everything down to the #undef below. Moving an arm across that boundary,
// adding one or deleting one has to move the matching per-region floor in this app's
// CMakeLists, which asserts the floors still sum to the whole suite. A boundary only ever
// moves between two ADJACENT registrations, so no arm changes place relative to another.
#if KICKOS_SELFTEST_REGION(1)
#define TAP_ADD(name, fn) tap::add(name, fn)
#else
#define TAP_ADD(name, fn) TAP_ELIDE(fn)
#endif
    // Core scheduler / sync / time: no test-only syscalls, runs on every board.
    TAP_ADD("svc_roundtrip", t_svc);
    TAP_ADD("fifo_order", t_fifo);
    TAP_ADD("preempt_on_ready", t_preempt);
    TAP_ADD("cpu_clock_hz", t_cpu_clock_hz);
    TAP_ADD("periph_clock_hz", t_periph_clock_hz);
    TAP_ADD("byte_ring", t_byte_ring);
    TAP_ADD("console_crlf", t_console_crlf);
    TAP_ADD("cap_dest", t_cap_dest);
    TAP_ADD("pinmux_set", t_pinmux_set);
    TAP_ADD("cpu_clock_set", t_cpu_clock_set);
    TAP_ADD("rr_interleave", t_rr);
    TAP_ADD("sleep_order", t_sleep);
    TAP_ADD("multi_wait", t_multi);
    TAP_ADD("sem_destroy", t_sem_destroy);
    TAP_ADD("sem_destroy_quiescent", t_sem_destroy_busy);
    TAP_ADD("sem_raii", t_sem_raii);
#undef TAP_ADD
// Region 2.
#if KICKOS_SELFTEST_REGION(2)
#define TAP_ADD(name, fn) tap::add(name, fn)
#else
#define TAP_ADD(name, fn) TAP_ELIDE(fn)
#endif
    // PI-mutex capability: production syscalls only, so runs on every board.
    TAP_ADD("mutex_basic", t_mutex_basic);
    TAP_ADD("mutex_pi_donation", t_mutex_pi);
    TAP_ADD("mutex_chain_boost", t_mutex_chain);
    TAP_ADD("mutex_owner_died", t_mutex_owner_died);
    TAP_ADD("mutex_deadlock", t_mutex_deadlock);
    TAP_ADD("mutex_close_owned", t_mutex_close_owned);
    TAP_ADD("mutex_multi_held", t_mutex_multi_held);
    TAP_ADD("mutex_unlock_errors", t_mutex_unlock_errors);
    TAP_ADD("mutex_owner_died_nowaiter", t_mutex_owner_died_nowaiter);
    TAP_ADD("mutex_deleg_refcount", t_mutex_deleg_refcount);
    TAP_ADD("prio_self_raise_lower", t_prio_self_raise_lower);
    TAP_ADD("prio_self_boosted", t_prio_self_boosted);
    // Endpoint IPC: production syscalls, so runs on every board.
    TAP_ADD("endpoint_rendezvous", t_endpoint_rendezvous);
    TAP_ADD("endpoint_reject", t_endpoint_reject);
    TAP_ADD("endpoint_rights", t_endpoint_rights);
    TAP_ADD("endpoint_epipe", t_endpoint_epipe);
    TAP_ADD("endpoint_dead", t_endpoint_dead);
    TAP_ADD("endpoint_handout", t_endpoint_handout);
#undef TAP_ADD
// Region 3.
#if KICKOS_SELFTEST_REGION(3)
#define TAP_ADD(name, fn) tap::add(name, fn)
#else
#define TAP_ADD(name, fn) TAP_ELIDE(fn)
#endif
    TAP_ADD("endpoint_handout_parked", t_endpoint_handout_parked);
    TAP_ADD("endpoint_zero_accept", t_endpoint_zero_accept);
    TAP_ADD("endpoint_send_timeout", t_endpoint_send_timeout);
    TAP_ADD("recv_timeout", t_recv_timeout);
    TAP_ADD("timed_arg_refusals", t_timed_arg_refusals);
    TAP_ADD("reply_recv_lens_clamp", t_reply_recv_lens_clamp);
    TAP_ADD("call_timeout_pending", t_call_timeout_pending);
    TAP_ADD("call_timeout_revert", t_call_timeout_revert);
    TAP_ADD_PINNED("call_timeout_reply", t_call_timeout_reply);
    TAP_ADD_PINNED("reply_stale_caller", t_reply_stale_caller);
    TAP_ADD("reply_abandoned_cap", t_reply_abandoned_cap);
    TAP_ADD("call_infoless_revert", t_call_infoless_revert);
    TAP_ADD("call_close_reply", t_call_close_reply);
    TAP_ADD("call_happy", t_call_happy);
    TAP_ADD("reply_recv_loop", t_reply_recv_loop);
    TAP_ADD("reply_recv_no_reply", t_reply_recv_no_reply);
    TAP_ADD("reply_recv_timeout", t_reply_recv_timeout);
#undef TAP_ADD
// Region 4.
#if KICKOS_SELFTEST_REGION(4)
#define TAP_ADD(name, fn) tap::add(name, fn)
#else
#define TAP_ADD(name, fn) TAP_ELIDE(fn)
#endif
    TAP_ADD("reply_recv_bad_ep_wakes_caller", t_reply_recv_bad_ep_wakes_caller);
    TAP_ADD("service_survives_client_fault", t_service_survives_client_fault);
#if defined(KICKOS_ENABLE_SELFTEST)
    TAP_ADD_IRQ("reply_recv_notify_park", t_reply_recv_notify_park);
    TAP_ADD_IRQ("reply_recv_notify", t_reply_recv_notify);
    TAP_ADD_IRQ("irq_wait_timeout", t_irq_wait_timeout);
#endif
#if defined(KICKOS_ENABLE_SELFTEST)
    TAP_ADD("call_reg_fastpath", t_call_reg_fastpath);
#endif
    TAP_ADD("call_from_root", t_call_from_root);
    TAP_ADD("call_truncation", t_call_truncation);
    TAP_ADD("call_double_reply", t_call_double_reply);
    TAP_ADD("call_server_death", t_call_server_death);
    TAP_ADD("call_prepop_death", t_call_prepop_death);
    TAP_ADD("call_donation", t_call_donation);
    TAP_ADD("call_donation_hold", t_call_donation_hold);
#undef TAP_ADD
// Region 5.
#if KICKOS_SELFTEST_REGION(5)
#define TAP_ADD(name, fn) tap::add(name, fn)
#else
#define TAP_ADD(name, fn) TAP_ELIDE(fn)
#endif
    TAP_ADD("call_donation_slow", t_call_donation_slow);
    TAP_ADD("call_donation_pending", t_call_donation_pending);
#if defined(KICKOS_ENABLE_SELFTEST)
    TAP_ADD("bus_device_slots", t_bus_device_slots);
    TAP_ADD("uart_service", t_uart_service);
#endif
    TAP_ADD("endpoint_crossdomain", t_endpoint_crossdomain);
#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
    TAP_ADD("endpoint_bound", t_endpoint_bound);
#endif
    // Console handover mechanism: production syscalls, every board.
    TAP_ADD("cap_index0", t_cap_index0);
    TAP_ADD("cap_chunk_span", t_cap_chunk_span);
    TAP_ADD("cap_gen_reuse", t_cap_gen_reuse);
    TAP_ADD("cap_child_width", t_cap_child_width);
    TAP_ADD("cap_reply_bound_fast", t_cap_reply_bound_fast);
    TAP_ADD("cap_reply_bound_slow", t_cap_reply_bound_slow);
    TAP_ADD("cap_reply_release_close", t_cap_reply_release_close);
    TAP_ADD("cap_reply_slot_reuse", t_cap_reply_slot_reuse);
#undef TAP_ADD
// Region 6.
#if KICKOS_SELFTEST_REGION(6)
#define TAP_ADD(name, fn) tap::add(name, fn)
#else
#define TAP_ADD(name, fn) TAP_ELIDE(fn)
#endif
    TAP_ADD("console_publish_priv", t_console_publish);
    TAP_ADD("shutdown_priv", t_shutdown_denied);
#if defined(KICKOS_ENABLE_SELFTEST)
    TAP_ADD("reboot_priv", t_reboot_denied);
#endif
    TAP_ADD("writable_global", t_writable_global);
    TAP_ADD("readable_global", t_readable_global);
    TAP_ADD("authority_cap", t_authority_cap);
    TAP_ADD("task_authority", t_task_authority);
    TAP_ADD("periph_enable_unheld", t_periph_enable_unheld);
    TAP_ADD("periph_reg_write_unheld", t_periph_reg_write_unheld);
#if KICKOS_ARCH_SIM
    TAP_ADD("periph_reg_write_mask", t_periph_reg_write_mask);
    TAP_ADD("window_list", t_window_list);
#endif
    TAP_ADD("privileged_spawn_refused", t_privileged_spawn_refused);
    TAP_ADD("thread_join", t_thread_join);
    TAP_ADD("join_stale_gen", t_join_stale_gen);
    TAP_ADD("join_timeout", t_join_timeout);
    TAP_ADD("task_exit_entry_return", t_task_exit_entry_return);
    TAP_ADD("task_exit_member_exit", t_task_exit_member_exit);
#undef TAP_ADD
// Region 7.
#if KICKOS_SELFTEST_REGION(7)
#define TAP_ADD(name, fn) tap::add(name, fn)
#else
#define TAP_ADD(name, fn) TAP_ELIDE(fn)
#endif
    TAP_ADD("task_dead_after_sweep", t_task_dead_after_sweep);
    TAP_ADD("task_dead_after_every_sweep", t_task_dead_after_every_sweep);
    TAP_ADD("task_handles", t_task_handles);
    TAP_ADD("task_member_refusals", t_task_member_refusals);
    TAP_ADD("task_creator_gate", t_task_creator_gate);
    TAP_ADD("task_group_kill", t_task_group_kill);
    TAP_ADD("prio_self_ceiling", t_prio_self_ceiling);
    TAP_ADD("task_watch_reports", t_task_watch_reports);
    TAP_ADD("thread_slay_window", t_thread_slay_window);
    TAP_ADD("thread_slay_gate", t_thread_slay_gate);
    TAP_ADD("thread_slay_timeout", t_thread_slay_timeout);
    TAP_ADD("task_slay_group", t_task_slay_group);
    TAP_ADD("task_slay_gate", t_task_slay_gate);
    TAP_ADD("task_slay_after_every_sweep", t_task_slay_after_every_sweep);
#undef TAP_ADD
// Region 8.
#if KICKOS_SELFTEST_REGION(8)
#define TAP_ADD(name, fn) tap::add(name, fn)
#else
#define TAP_ADD(name, fn) TAP_ELIDE(fn)
#endif
    // Neither needs the software-inject syscall, and notify_no_line needs no LINE at all:
    // both must run on every posture.
    TAP_ADD("notify_no_line", t_notify_no_line);
    TAP_ADD("notify_bind_busy", t_notify_bind_busy);
#if defined(KICKOS_ENABLE_SELFTEST)
    // Need the software-inject syscall (compiled out of the production ABI).
    TAP_ADD_IRQ("irq_thread_ctx", t_irq);
    TAP_ADD_IRQ("irq_as_event", t_irqdrv);
    TAP_ADD_IRQ("irq_mask_coalesce", t_irq_mask);
    TAP_ADD_IRQ("irq_discard", t_irq_discard);
    TAP_ADD_IRQ("irq_autorearm", t_irq_autorearm);
    TAP_ADD_IRQ("irq_phantom_wake", t_irq_phantom);
    TAP_ADD_IRQ("irq_ownership", t_irq_ownership);
    TAP_ADD("irq_spurious", t_irq_spurious);
    TAP_ADD_IRQ("irq_stale_register", t_irq_stale_register);
    TAP_ADD_IRQ("irq_claim_gate", t_irq_claim_gate);
    TAP_ADD_IRQ("irq_reclaim", t_irq_reclaim);
    TAP_ADD_IRQ("irq_server_handover", t_irq_server_handover);
    TAP_ADD("irq_notify_already", t_irq_notify_already);
#if KICKOS_KERNEL_CORES > 1
    TAP_ADD("irq_cross_core_wake", t_irq_cross_core_wake);
    TAP_ADD("irq_reclaim_stale_raise", t_irq_reclaim_stale_raise);
#endif
#endif
#undef TAP_ADD
// Region 9.
#if KICKOS_SELFTEST_REGION(9)
#define TAP_ADD(name, fn) tap::add(name, fn)
#else
#define TAP_ADD(name, fn) TAP_ELIDE(fn)
#endif
#if KICKOS_HAVE_ASPACE
    TAP_ADD("caller_stack_arena", t_caller_stack_arena);
#endif
    // Here, not beside mem_self_grant: see the run-order note at t_selfgrant_nonpow2.
    TAP_ADD("mem_self_grant_nonpow2", t_selfgrant_nonpow2);
#if not KICKOS_HAVE_ASPACE
    TAP_ADD("region_mode", t_region_mode);
#endif
    TAP_ADD("domain_share", t_domain_share);
#if KICKOS_MEMORY_ENFORCED
    // Both backends, and neither a flat board: there is no ownership to breach where
    // nothing is enforced, and every case here would be admitted.
    TAP_ADD("cross_task_block", t_cross_task_block);
#endif
    TAP_ADD("mmio_grant", t_mmio_grant);
#if KICKOS_FAULT_ISOLATION
    TAP_ADD("task_exit_driver_trap", t_task_exit_driver_trap);
#endif
#if KICKOS_MEMORY_ENFORCED && KICKOS_FAULT_ISOLATION
    TAP_ADD("window_memory_ro", t_window_memory_ro);
    TAP_ADD("task_exit_member_fault", t_task_exit_member_fault);
    TAP_ADD("task_exit_entry_killed", t_task_exit_entry_killed);
    TAP_ADD("task_exit_cancelled_fault", t_task_exit_cancelled_fault);
    TAP_ADD("task_exit_implicit_fault", t_task_exit_implicit_fault);
#endif
#if defined(KICKOS_ENABLE_SELFTEST) && (KICKOS_HAVE_MPU || defined(KICKOS_SELFTEST_SPARE_DEV))
    TAP_ADD("window_get", t_window_get);
#endif
#if defined(KICKOS_ENABLE_SELFTEST) && not KICKOS_HAVE_ASPACE
    TAP_ADD("ram_alloc_zeroed", t_ram_alloc_zeroed);
    TAP_ADD("uncached_grant_sync", t_uncached_grant_sync);
#endif
    // Last of its region: a stack that fits spends arena the probes above need on microbit.
    TAP_ADD("caller_stack", t_caller_stack);
#undef TAP_ADD
// Region 10.
#if KICKOS_SELFTEST_REGION(10)
#define TAP_ADD(name, fn) tap::add(name, fn)
#else
#define TAP_ADD(name, fn) TAP_ELIDE(fn)
#endif
#if KICKOS_HAVE_MPU
    TAP_ADD("stackbase_arena", t_stackbase_arena);
#if defined(KICKOS_ENABLE_SELFTEST)
    TAP_ADD("grant_reserved", t_grant_reserved);
    TAP_ADD("dev_window_exclusive", t_dev_window_exclusive);
#endif
#endif
#if KICKOS_HAVE_ASPACE && defined(KICKOS_ENABLE_SELFTEST)
    TAP_ADD("cap_objects", t_cap_objects);
    TAP_ADD("cap_map", t_cap_map);
    TAP_ADD("cap_map_over_stack", t_cap_map_over_stack);
    TAP_ADD("stack_grant_refused", t_stack_grant_refused);
    TAP_ADD("stack_guard_intact", t_stack_guard_intact);
    TAP_ADD("stack_handoff_refused", t_stack_handoff_refused);
    TAP_ADD("stack_slot_returns", t_stack_slot_returns);
    TAP_ADD("cap_map_pins_run", t_cap_map_pins_run);
    TAP_ADD("cap_share", t_cap_share);
    TAP_ADD("aspace_seam", t_aspace_seam);
    TAP_ADD("aspace_model", t_aspace_model);
    TAP_ADD("aspace_map_cycle", t_aspace_map_cycle);
    TAP_ADD("aspace_translate", t_aspace_translate);
    TAP_ADD("aspace_refusals", t_aspace_refusals);
    TAP_ADD("aspace_span", t_aspace_span);
    TAP_ADD("aspace_acquire_dup", t_aspace_acquire_dup);
    TAP_ADD("aspace_balance", t_aspace_balance);
    TAP_ADD("aspace_domain_balance", t_aspace_domain_balance);
    TAP_ADD("aspace_forced_unwind", t_aspace_forced_unwind);
    TAP_ADD("aspace_churn", t_aspace_churn);
    TAP_ADD("stack_is_frames", t_stack_is_frames);
    TAP_ADD("aspace_two_spaces_same_grant", t_aspace_two_spaces_same_grant);
    TAP_ADD("aspace_two_spaces_no_grant", t_aspace_two_spaces_no_grant);
    TAP_ADD("process_private_data", t_process_private_data);
    TAP_ADD("task_siblings_share", t_task_siblings_share);
    TAP_ADD("task_handoff_readback", t_task_handoff_readback);
    TAP_ADD("task_handoff_donor_exits", t_task_handoff_donor_exits);
    TAP_ADD("task_handoff_slice", t_task_handoff_slice);
    TAP_ADD("reservation_teardown", t_reservation_teardown);
    TAP_ADD("frame_scrub_cross_task", t_frame_scrub_cross_task);
    TAP_ADD("spawn_refusal_frees_task", t_spawn_refusal_frees_task);
    TAP_ADD("spawn_refusal_frees_donor", t_spawn_refusal_frees_donor);
    TAP_ADD("parked_frame_hostile", t_parked_frame_hostile);
    TAP_ADD("split_access", t_split_access);
    TAP_ADD("process_ipc_same_addr", t_process_ipc_same_addr);
    TAP_ADD("process_call_reply", t_process_call_reply);
    TAP_ADD("grant_kernel_word_refused", t_grant_kernel_word_refused);
    TAP_ADD("self_grant_retype", t_self_grant_retype);
    TAP_ADD("uncached_alias_sync", t_uncached_alias_sync);
    TAP_ADD("uncached_teardown", t_uncached_teardown);
    TAP_ADD("presync_retried", t_presync_retried);
    TAP_ADD_IRQ("presync_race", t_presync_race);
    TAP_ADD_IRQ("presync_flip", t_presync_flip);
    TAP_ADD_IRQ("presync_slain", t_presync_slain);
    TAP_ADD("presync_staged", t_presync_staged);
    TAP_ADD("presync_cancel", t_presync_cancel);
    TAP_ADD_IRQ("presync_late_params", t_presync_late_params);
    TAP_ADD_IRQ("out_of_lock_windows", t_out_of_lock_windows);
    TAP_ADD("reent_seating", t_reent_seating);
    TAP_ADD("aspace_acquire_balance", t_aspace_acquire_balance);
    TAP_ADD("map_tlbi_elided", t_map_tlbi_elided);
#if KICKOS_KERNEL_CORES > 1 && defined(__x86_64__)
    TAP_ADD("shootdown_per_change", t_shootdown_per_change);
#endif
    TAP_ADD("aspace_active_cores", t_aspace_active_cores);
    TAP_ADD("app_pointers_relocated", t_app_pointers_relocated);
    TAP_ADD("recv_buf_unmapped", t_recv_buf_unmapped);
    TAP_ADD("frame_run_slot_recycle", t_frame_run_slot_recycle);
    TAP_ADD("call_reply_undisclosed", t_call_reply_undisclosed);
    // Ahead of process_data_template, while main's own data pages are still a live source.
    TAP_ADD("process_data_from_image", t_process_data_from_image);
    // Last of the block: it drops the space holding the image's own data pages for good, and
    // every process created after it copies the snapshot instead.
    TAP_ADD("process_data_template", t_process_data_template);
#endif
#if defined(KICKOS_ENABLE_SELFTEST)
    // Not under the address-space gate: the doorbell is a property of the machine's cores.
    TAP_ADD("doorbell_xpoke", t_doorbell_xpoke);
    TAP_ADD_IRQ("irq_kernel_line_reserved", t_irq_kernel_line_reserved);
    TAP_ADD("prio_ceiling_refused", t_prio_ceiling_refused);
    TAP_ADD("prio_ceiling_narrow_only", t_prio_ceiling_narrow_only);
#endif
#if defined(KICKOS_ENABLE_SELFTEST) && KICKOS_KERNEL_CORES > 1
    TAP_ADD("pin_places", t_pin_places);
    TAP_ADD("pin_wrong_core_never", t_pin_wrong_core_never);
    TAP_ADD("unpin_restores", t_unpin_restores);
    TAP_ADD("affinity_zero_defaults", t_affinity_zero_defaults);
    TAP_ADD("affinity_undriven_refused", t_affinity_undriven_refused);
    TAP_ADD("migrate_running", t_migrate_running);
    TAP_ADD("resched_reaches_pinned_caller", t_resched_reaches_pinned_caller);
    TAP_ADD("prio_self_lower_moves_waiter", t_prio_self_lower_moves_waiter);
    TAP_ADD("pin_same_task_ok", t_pin_same_task_ok);
    TAP_ADD("pin_cross_task_refused", t_pin_cross_task_refused);
    TAP_ADD("grant_narrows", t_grant_narrows);
    TAP_ADD("grant_wider_refused", t_grant_wider_refused);
    TAP_ADD("grant_after_member_refused", t_grant_after_member_refused);
    TAP_ADD("grant_second_narrow_only", t_grant_second_narrow_only);
    TAP_ADD("grant_inherited_by_child_task", t_grant_inherited_by_child_task);
    TAP_ADD("isolated_single_grant_ok", t_isolated_single_grant_ok);
    TAP_ADD("affinity_dead_handle_refused", t_affinity_dead_handle_refused);
    TAP_ADD("isolated_unpinned_never", t_isolated_unpinned_never);
    TAP_ADD("isolated_unpin_excludes", t_isolated_unpin_excludes);
    TAP_ADD("isolated_takes_pinned", t_isolated_takes_pinned);
    TAP_ADD("isolated_mixed_mask_ok", t_isolated_mixed_mask_ok);
#if KICKOS_HAVE_ASPACE
    // After every arm that reserves for life, so main's space is as full as the crowds meet it.
    TAP_ADD("crowd_room_in_root_space", t_crowd_room_in_root_space);
#endif
    TAP_ADD("slice_preempts_every_core", t_slice_preempts_every_core);
    TAP_ADD("threads_reach_every_core", t_threads_reach_every_core);
    TAP_ADD("reent_per_thread_cores", t_reent_per_thread_cores);
#if defined(__x86_64__)
    TAP_ADD("fp_enabled_every_core", t_fp_enabled_every_core);
#endif
#endif
#if KICKOS_HAVE_ASPACE && defined(KICKOS_ENABLE_SELFTEST) && KICKOS_FAULT_ISOLATION
    TAP_ADD("fault_kills_task", t_fault_kills_task);
    TAP_ADD("kernel_state_unreachable", t_kernel_state_unreachable);
#if defined(KICKOS_SELFTEST_SPARE_DEV)
    TAP_ADD("window_addr", t_window_addr);
#endif
#if defined(__x86_64__)
    TAP_ADD("port_window", t_port_window);
    TAP_ADD("vector_survives_block", t_vector_survives_block);
    TAP_ADD("vector_survives_preempt", t_vector_survives_preempt);
#if KICKOS_KERNEL_CORES > 1
    TAP_ADD("vector_survives_migrate", t_vector_survives_migrate);
#endif
    TAP_ADD("vector_starts_clean", t_vector_starts_clean);
    TAP_ADD("vector_fault_contained", t_vector_fault_contained);
#endif
#endif
#if defined(KICKOS_ENABLE_SELFTEST) && KICKOS_AMP_NODE
    TAP_ADD("amp_window", t_amp_window);
    // Beside amp_window, which stomps the same ring: this one asserts the record lifetime that
    // ring's resynchronisation decides.
    TAP_ADD("amp_reset_record", t_amp_reset_record);
    // With it, and before every arm that spends a reply slot of its own: these three answer the
    // other half of that resynchronisation and the producer's own bound. Each owns its
    // reply-ring precondition, so their place here rests on nothing that ran before.
    TAP_ADD("amp_far_reset_answers", t_amp_far_reset_answers);
    TAP_ADD("amp_far_answer_deferred", t_amp_far_answer_deferred);
    TAP_ADD("amp_far_tail_recovery", t_amp_far_tail_recovery);
    // Before every arm that spends one: they all rest on the derivation this drives.
    TAP_ADD("amp_port_seating", t_amp_port_seating);
    TAP_ADD("amp_port_unnamed", t_amp_port_unnamed);
#if KICKOS_AMP_OWN_IMAGE && KICKOS_MEMORY_ENFORCED && KICKOS_AMP_USER_SHARE_SIZE != 0
    TAP_ADD("amp_share_seated", t_amp_share_seated);
    TAP_ADD("amp_share_window", t_amp_share_window);
#endif
    // Both need a peer that is running: one waits for its answer, the other parks a caller on a
    // far endpoint for the whole forge. Registered unconditionally, each skipping by name where
    // no peer answers.
    TAP_ADD("amp_far_call", t_amp_far_call);
    // After amp_far_call, whose round must not race a forged reply.
    TAP_ADD("amp_far_reply_guard", t_amp_far_reply_guard);
    // After the guard, whose caller must park with no zero-length answer racing it.
    TAP_ADD("amp_far_reply_empty", t_amp_far_reply_empty);
    TAP_ADD("amp_far_service", t_amp_far_service);
#if KICKOS_AMP_OWN_IMAGE
    // Needs a peer that is running, and skips by name where none answers.
    TAP_ADD("amp_share_crossing", t_amp_share_crossing);
#endif
    // Before the two arms that count reply publications of their own: this node's reply ring
    // at the peer holds KOS_AMP_RING_SLOTS and nothing drains it on a node booted alone.
    TAP_ADD("amp_far_refusal_answered", t_amp_far_refusal_answered);
    TAP_ADD("amp_deferred_doorbell", t_amp_deferred_doorbell);
    TAP_ADD("amp_inbound_reply", t_amp_inbound_reply);
    TAP_ADD("amp_far_undisclosed", t_amp_far_undisclosed);
    TAP_ADD("amp_far_infoless", t_amp_far_infoless);
    TAP_ADD("amp_reply_band", t_amp_reply_band);
    // Publishes nothing, so its place is free of the reply-ring ordering above.
    TAP_ADD("amp_mint_reply_port", t_amp_mint_reply_port);
    // Owns the ring it fills and gives back, so its place here is free of the ordering above.
    TAP_ADD("amp_reply_reserve", t_amp_reply_reserve);
    TAP_ADD("amp_probe_crossing_holder", t_amp_probe_crossing_holder);
    TAP_ADD("amp_far_deliver_fault", t_amp_far_deliver_fault);
    // Last two of the block: both fill the endpoint pool, so an arm run while either holds
    // the slots would be refused one, and each spends one of the partition's capabilities.
    // The local one first: it closes the port every receiving arm above needs.
    TAP_ADD("amp_local_port_slot_held", t_amp_local_port_slot_held);
    TAP_ADD("amp_far_slot_reuse", t_amp_far_slot_reuse);
#endif
    TAP_ADD("ipc_one_buffer_both_ends", t_ipc_one_buffer_both_ends);
    TAP_ADD("confused_deputy", t_confused_deputy);
    // After every arm that reads the console's state: these two publish it and leave it
    // reclaimed, polled for every line after.
    TAP_ADD("console_publish_handout", t_console_publish_handout);
    TAP_ADD("console_publish_narrow", t_console_publish_narrow);
    // Last, deliberately: the blocks it buys are never returned (bump allocator), so
    // running it earlier would spend arena the tests above still need on a small board.
    TAP_ADD("mem_self_grant", t_selfgrant);
#undef TAP_ADD
// A region this file cuts but the build does not know about is elided from EVERY image and
// runs nowhere, which no plan check can see.
#if KICKOS_SELFTEST_REGIONS != 10
#error "this file cuts the registry into ten regions; say so in its CMakeLists"
#endif

    // The failure count is the system's exit status.
    exit(tap::run_all());
}
