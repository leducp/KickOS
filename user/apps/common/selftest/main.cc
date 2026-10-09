// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Unprivileged userspace tests with TAP output. Ordering checks use a semaphore-protected event
// log, not console order. The registration list stays in this file: its TAP_ADD redefinitions
// cut it into six regions, which a small board builds as separate images.

#include "selftest.h"

#include <kickos/arch/arch.h> // arch_syscall
#include <kickos/arch/mpu_overlap.h>
#if defined(__aarch64__)
#include <kickos/arch/gic_ppi.h>
#endif
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

// How the MPU a spawn's regions are held to decides an overlap, or -1 where no region set is.
#if KICKOS_MEMORY_ENFORCED and KICKOS_HAVE_MPU and not KICKOS_HAVE_ASPACE
#define ST_MPU_OVERLAP ARCH_MPU_OVERLAP
#else
#define ST_MPU_OVERLAP (-1)
#endif

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
        kos_sem_wait(CH_LOCK, KOS_TIMEOUT_NONE);
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

    // A red arm can leave tokens its workers posted after main stopped waiting for them.
    void done_drain()
    {
        while (kos_sem_wait(g_done, 0) == 0)
        {
        }
    }

    // A thread that waits several times shares one deadline across its waits.
    uint64_t deadline_in(uint32_t us)
    {
        return kos_clock_now() + uint64_t{us} * 1000u;
    }
    uint32_t left_us_until(uint64_t deadline)
    {
        uint64_t const now = kos_clock_now();
        if (now >= deadline)
        {
            return 0;
        }
        return static_cast<uint32_t>((deadline - now) / 1000u);
    }
    // A creator's whole run ends before the TX_WAIT_US waits of the workers it spawns give up; the
    // arm's hold waits longer than it.
    constexpr uint32_t CREATOR_US = STALL_TOLERANT_US - STALL_TOLERANT_US / 4;
    constexpr uint32_t CREATOR_HOLD_US = 2 * STALL_TOLERANT_US;

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
        kos_sem_wait(gate, KOS_TIMEOUT_NONE);
        kos_sem_post(gate);
    }

    char arg_char(void* arg)
    {
        return static_cast<char>(reinterpret_cast<uintptr_t>(arg));
    }


    // A short count leaves the rest of s to the caller, and -KOS_EBUSY all of it to this thread's
    // stdout; any other refusal leaves nothing to send.
    void write_rest(char const* s, size_t n, int32_t took)
    {
        size_t sent = 0;
        if (took > 0)
        {
            sent = static_cast<size_t>(took);
        }
        else if (took == -KOS_EBUSY)
        {
            (void)kickos::stdout_write(s, n);
            return;
        }
        else
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
        return took > 0 and static_cast<size_t>(took) <= n;
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
        int32_t const took = KICKOS_KCONSOLE_MEASURED(ANSWER, s, n);
        TAP_CHECK(console_count_ok(took, n));
        write_rest(s, n, took);
        // A len-0 write is a legitimate 0 (sys.h).
        TAP_CHECK(KICKOS_KCONSOLE_MEASURED(ANSWER, s, 0) == 0);
        // The prefix must itself be a whole line or the TAP stream is malformed.
        char const* pfx = "# [svc] len-honoured prefix\nTRAILING-MUST-NOT-APPEAR";
        size_t const cut = strlen("# [svc] len-honoured prefix\n");
        int32_t const cut_took = KICKOS_KCONSOLE_MEASURED(ANSWER, pfx, cut);
        TAP_CHECK(console_count_ok(cut_took, cut));
        write_rest(pfx, cut, cut_took);

        // Index 0 is the stdout slot, in either posture. A zero-length send puts no byte on
        // the wire in both.
        int32_t const stdout_seated = kos_send_timed(KOS_CAP_STDOUT, "", 0, STALL_TOLERANT_US);
        if (stdout_seated == -KOS_EBADF)
        {
            // Before a publish nothing is seated there, so a send fails cleanly rather than
            // resolving a stale object.
            TAP_CHECK(kos_send_timed(KOS_CAP_STDOUT, "x", 1, STALL_TOLERANT_US) == -KOS_EBADF);
        }
        else
        {
            // After one, a send-only endpoint: the zero-length signal completes only with a
            // live receiver, the one place the suite proves console output is ACKed.
            TAP_CHECK(stdout_seated == 0);
        }
    }

    // --- FIFO ordering ---------------------------------------------------------
    void fifo_worker(void* arg) // caps: done@1, lock@2
    {
        log_put(arg_char(arg));
    }
    void t_fifo()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        TAP_ASK(.workers = 2);
        log_reset();
        kos::thread::Handle a;
        kos::thread::Handle b;
        ArmHold hold;
        TAP_HOLD(hold.thread(&a) and hold.thread(&b));
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        a = kos::thread::create_caps(fifo_worker, reinterpret_cast<void*>('A'), "fifoA", 10, caps,
                                     2);
        b = kos::thread::create_caps(fifo_worker, reinterpret_cast<void*>('B'), "fifoB", 10, caps,
                                     2);
        TAP_CHECK(a.valid() and b.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(log_eq("AB"));
    }

    // --- Priority preempt on ready (thread-ctx sem post) -----------------------
    kos_cap_t g_go = KOS_CAP_NONE;
    void preempt_high(void*) // caps: done@1, lock@2, go@3
    {
        kos_sem_wait(CH_AUX, KOS_TIMEOUT_NONE); // g_go
        log_put('H');
    }
    void preempt_low(void*) // caps: done@1, lock@2, go@3
    {
        log_put('l');
        kos_sem_post(CH_AUX); // g_go
        log_put('L');
    }
    void t_preempt()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        TAP_ASK(.workers = 2, .sems = 1);
        log_reset();
        kos::thread::Handle hi;
        kos::thread::Handle lo;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_go) and hold.thread(&hi) and hold.thread(&lo));
        TAP_CHECK(kos_sem_create(0, &g_go) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_go, CH_FULL}};
        hi = kos::thread::create_caps(preempt_high, nullptr, "high", 20, caps, 3);
        lo = kos::thread::create_caps(preempt_low, nullptr, "low", 8, caps, 3);
        TAP_CHECK(hi.valid() and lo.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(log_eq("lHL"));
    }

    void t_periph_clock_hz()
    {
        // A base no backend models returns 0 on EVERY target: the dispatch path and the
        // fallback plumbing reach the arch seam.
        uint32_t const bogus = kos_periph_clock_hz(0xDEAD0000u);
        TAP_CHECK(bogus == 0u);
        TAP_CHECK(bogus == kos_periph_clock_hz(0xDEAD0000u));
    }

    // kos_cpu_clock_set is gated on AUTH_PSTATE: the gate returns the sentinel 0 ("cannot
    // change") to a caller that does not hold it, with NO retune. Neither main nor its child
    // holding no authority holds it.
    uint64_t g_clkset_low = 1;
    uint64_t g_clkset_mid = 1;
    uint64_t g_clkset_max = 1;
    void clkset_unpriv_worker(void*) // UNPRIVILEGED
    {
        g_clkset_low = kos_cpu_clock_set(KOS_PSTATE_LOW);
        g_clkset_mid = kos_cpu_clock_set(KOS_PSTATE_MID);
        g_clkset_max = kos_cpu_clock_set(KOS_PSTATE_MAX);
    }
    void t_cpu_clock_set()
    {
        uint64_t const before = kos_cpu_clock_hz();
        // 0 is a backend with no silicon core clock (host sim, QEMU virt); a real core reports
        // a plausible rate.
        TAP_CHECK(before == 0u or before >= 1000000u);
        TAP_ASK(.workers = 1);
        uint64_t const t0 = kos_clock_now();
        g_clkset_low = 1;
        g_clkset_mid = 1;
        g_clkset_max = 1;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.thread(&w));
        w = kos::thread::create_caps(clkset_unpriv_worker, nullptr, "clkset", 10, nullptr, 0);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(kos_cpu_clock_set(KOS_PSTATE_LOW) == 0u);
        TAP_CHECK(g_clkset_low == 0u);
        TAP_CHECK(g_clkset_mid == 0u);
        TAP_CHECK(g_clkset_max == 0u);
        TAP_CHECK(kos_cpu_clock_hz() == before);
        TAP_CHECK(kos_clock_now() >= t0);
    }

    // Every bit of a notification.
    constexpr uint32_t NOTE_ALL = 0xFFFFFFFFu;

#if defined(KICKOS_ENABLE_SELFTEST)
    // No arm raises an unclaimed line: the raise needs a claim, and irqquiesce holds the
    // unclaimed line's computation.
    // A line with no hardware source. ISR delivery uses the binding passed as its argument,
    // not the interrupted thread's capability table.
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

    void irq_waiter(void*) // caps: done@1, lock@2, line(WAIT)@3, note@4
    {
        auto note = kos::Notification::adopt(CH_NOTE);
        note.bind();
        note.wait(NOTE_ALL);
        log_put('W');
    }
    int g_irq_raised = -99;
    void irq_raiser(void*) // caps: done@1, lock@2, line(SIGNAL)@3
    {
        log_put('i');
        g_irq_raised = kos_irq_raise(CH_AUX);
        log_put('r');
    }
    void t_irq()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        log_reset();
        g_irq_raised = -99;
        kos_cap_t irq = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        kos::thread::Handle w;
        kos::thread::Handle inj;
        ArmHold hold;
        TAP_HOLD(hold.cap(&irq) and hold.cap(&note) and hold.thread(&w) and hold.thread(&inj));
        TAP_ASK_LINE(&irq, .workers = 2, .notifies = 1, .irq_line = IRQ_CTX_LINE);
        // The line must signal SOMEWHERE before it can be armed: an unattached line is one
        // kos_irq_ack refuses, because opening it would drop every raise.
        note = notify_for_line(irq);
        TAP_CHECK(note != KOS_CAP_NONE);
        // Arm explicitly because the raise may precede the waiter's first wait.
        kos_irq_ack(irq);
        kos_cap_grant wcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {irq, KOS_CAP_WAIT},
                                 {note, CH_FULL}};
        kos_cap_grant icaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {irq, KOS_CAP_SIGNAL}};
        w = irq_spawn(irq_waiter, nullptr, "irqW", 15, wcaps, 4);
        inj = kos::thread::create_caps(irq_raiser, nullptr, "irqI", 8, icaps, 3);
        TAP_CHECK(w.valid() and inj.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(g_irq_raised == 0);
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

    // No order of two logged characters is asserted: a slice's length is not established.
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
    }
    void t_rr()
    {
        TAP_ASK(.workers = 2, .sems = 1);
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
        kos::thread::Handle a;
        kos::thread::Handle b;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_gate) and hold.thread(&a) and hold.thread(&b));
        TAP_CHECK(kos_sem_create(0, &g_gate) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL},
                                {g_gate, CH_FULL}};
        // Must stay UNPRIVILEGED: that is what exercises the region reload per slice.
        a = kos::thread::create_caps(rr_burner, nullptr, "rrA", 10,
                                     caps, 3, KOS_POLICY_RR, static_cast<uint32_t>(quantum),
                                     /*privileged=*/false);
        b = kos::thread::create_caps(rr_spinner, nullptr, "rrB", 10,
                                     caps, 3, KOS_POLICY_RR, static_cast<uint32_t>(quantum),
                                     /*privileged=*/false);
        TAP_CHECK(a.valid() and b.valid());
        TAP_CHECK(kos_sem_wait(g_done, STALL_TOLERANT_US) == 0);
        TAP_CHECK(kos_sem_wait(g_done, STALL_TOLERANT_US) == 0);
        stage_release();
        TAP_CHECK(hold.joined());
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
    }
    void t_sleep()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        TAP_ASK(.workers = 2);
        log_reset();
        g_sleep_due_s = 0;
        g_sleep_due_l = 0;
        kos::thread::Handle l;
        kos::thread::Handle s;
        ArmHold hold;
        TAP_HOLD(hold.thread(&l) and hold.thread(&s));
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        l = kos::thread::create_caps(sleeper, reinterpret_cast<void*>(uintptr_t{40}), "sleepL", 10,
                                     caps, 2);
        s = kos::thread::create_caps(sleeper, reinterpret_cast<void*>(uintptr_t{10}), "sleepS", 10,
                                     caps, 2);
        TAP_CHECK(l.valid() and s.valid());
        TAP_CHECK(hold.joined());
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
    void multi_worker(void* arg) // caps: done@1, lock@2, multi@3
    {
        kos_sem_wait(CH_AUX, KOS_TIMEOUT_NONE); // g_multi
        log_put(arg_char(arg));
    }
    void t_multi()
    {
        TAP_ASK(.workers = 2, .sems = 1);
        log_reset();
        kos::thread::Handle a;
        kos::thread::Handle b;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_multi) and hold.thread(&a) and hold.thread(&b));
        TAP_CHECK(kos_sem_create(0, &g_multi) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_multi, CH_FULL}};
        a = irq_spawn(multi_worker, reinterpret_cast<void*>('A'), "multiA", TAP_PRIO_PARKS, caps,
                      3);
        b = irq_spawn(multi_worker, reinterpret_cast<void*>('B'), "multiB", TAP_PRIO_PARKS, caps,
                      3);
        TAP_CHECK(a.valid() and b.valid());
        kos_yield();
        kos_sem_post(g_multi);
        kos_sem_post(g_multi);
        TAP_CHECK(hold.joined());
        TAP_CHECK(count('A') == 1 and count('B') == 1);
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // --- Tier-1 IRQ-as-event: unprivileged userspace driver --------------------
    kos_cap_t g_irqdrv_done = KOS_CAP_NONE;
    // One ready-handshake handle shared by every tier-1 IRQ arm below; safe only because
    // they are strictly sequential and each creates and destroys it.
    kos_cap_t g_irq_ready = KOS_CAP_NONE;
    void* g_mmio = nullptr; // fake device MMIO word, granted to the driver
    constexpr uint32_t IRQDRV_BLK = 4u * sizeof(int);
    constexpr RamAsk IRQDRV_RAM = {IRQDRV_BLK};
    // Word 0 is the device, words 1..3 are what the driver saw. The block is the driver's
    // domain grant, making it a task of its own: an app global it wrote would be its own
    // copy, so the reading comes back only through this block.
    constexpr int IRQ_LINE = KICKOS_IRQ_FREE_BASE + 1;
    // Above the floor others_parked() drops main to, so an absence check waits these drivers out.
    constexpr uint8_t IRQ_DRV_PRIO = KICKOS_PRIO_MIN + 1;

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
        // main plays the device, so it needs write access to a block it reserved. App
        // static data will not do: it sits outside the arena and cannot be granted to the
        // driver.
        g_mmio = st_ram<IRQDRV_RAM, 0>();
        TAP_CHECK(g_mmio != nullptr);
        // Without this grant the writes below fault: main does not reach its own arena
        // allocations.
        TAP_CHECK(kos_mem_self_grant(g_mmio, IRQDRV_BLK, 0) == 0);
        for (int i = 0; i < 4; i++)
        {
            static_cast<volatile int*>(g_mmio)[i] = 0;
        }
        // main must mint the line (the suite declares KOS_AUTH_IRQ); a worker runs at
        // authority 0 and cannot claim for itself, so it gets a WAIT-only copy.
        kos_cap_t irq = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        kos::thread::Handle drv;
        ArmHold hold;
        TAP_HOLD(hold.cap(&irq) and hold.cap(&note) and hold.cap(&g_irqdrv_done)
                  and hold.cap(&g_irq_ready) and hold.thread(&drv));
        TAP_ASK_LINE(&irq, .workers = 1, .sems = 2, .notifies = 1, .irq_line = IRQ_LINE);
        TAP_CHECK(kos_sem_create(0, &g_irqdrv_done) == 0);
        TAP_CHECK(kos_sem_create(0, &g_irq_ready) == 0);
        note = notify_for_line(irq);
        TAP_CHECK(note != KOS_CAP_NONE);
        // A claim leaves the line MASKED and the ready handshake fires BEFORE the driver's
        // first wait, so arm the line here: otherwise a raise can land on a masked line
        // and the driver's first arm discards it.
        kos_irq_ack(irq);
        kos_cap_grant caps[] = {{g_irqdrv_done, CH_FULL},
                                {g_irq_ready, CH_FULL},
                                {irq, KOS_CAP_WAIT},
                                {note, CH_FULL}};
        drv = irq_spawn(irq_driver, nullptr, "irqdrv", 15, caps, 4,
                        KOS_POLICY_FIFO, 0, /*privileged=*/false, g_mmio, IRQDRV_BLK);
        TAP_CHECK(drv.valid());
        // The driver is the notification's sole holder, so its exit frees it.
        TAP_CHECK(hold.close(&note) == 0);
        TAP_CHECK(kos_sem_wait(g_irq_ready, STALL_TOLERANT_US) == 0);
        for (int i = 1; i <= 3; i++)
        {
            *static_cast<volatile int*>(g_mmio) = 0x100 + i;
            TAP_CHECK(kos_irq_raise(irq) == 0);
            TAP_CHECK(kos_sem_wait(g_irqdrv_done, STALL_TOLERANT_US) == 0);
        }
        TAP_CHECK(hold.joined());
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
            kos_sem_wait(CH_READY, KOS_TIMEOUT_NONE); // main holds the masked window open
            irq.ack();
            kos_sem_post(CH_DONE);  // phase B: the line is ARMED again
        }
    }
    void t_irq_mask()
    {
        g_mask_serviced = 0;
        kos_cap_t irq = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        kos::thread::Handle drv;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_irq_ready) and hold.cap(&irq) and hold.cap(&note)
                  and hold.thread(&drv));
        TAP_ASK_LINE(&irq, .workers = 1, .sems = 1, .notifies = 1, .irq_line = MASK_LINE);
        TAP_CHECK(kos_sem_create(0, &g_irq_ready) == 0);
        note = notify_for_line(irq);
        TAP_CHECK(note != KOS_CAP_NONE);
        kos_irq_ack(irq); // arm the freshly-claimed (masked) line
        kos_cap_grant caps[] = {{g_done, CH_FULL},
                                {g_irq_ready, CH_FULL},
                                {irq, KOS_CAP_WAIT},
                                {note, CH_FULL}};
        drv = irq_spawn(mask_driver, nullptr, "maskdrv", IRQ_DRV_PRIO, caps, 4);
        TAP_CHECK(drv.valid());
        TAP_CHECK(hold.close(&note) == 0);
        // Driver is bound to the object, about to wait
        TAP_CHECK(kos_sem_wait(g_irq_ready, STALL_TOLERANT_US) == 0);

        TAP_CHECK(kos_irq_raise(irq) == 0);
        // Phase A: serviced once, stopped before its ack, so masked from here
        TAP_CHECK(kos_sem_wait(g_done, STALL_TOLERANT_US) == 0);
        TAP_CHECK(kos_irq_raise(irq) == 0);
        TAP_CHECK(kos_irq_raise(irq) == 0);
        kos_sem_post(g_irq_ready); // release the window; the driver acks
        // The latch half: a raise taken while the line was masked was kept and redelivered
        // at the ack. main raised nothing after the gate opened, so the second service can
        // only be a redelivery. Two posts: the ack's phase B, then that service's phase A.
        TAP_CHECK(kos_sem_wait(g_done, STALL_TOLERANT_US) == 0);
        TAP_CHECK(kos_sem_wait(g_done, STALL_TOLERANT_US) == 0);
        bool const latched = (g_mask_serviced == 2);

        kos_sem_post(g_irq_ready);
        // Phase B: acked and ARMED, so a raise below cannot beat the ack
        TAP_CHECK(kos_sem_wait(g_done, STALL_TOLERANT_US) == 0);
        bool one_deep = true;
#if KICKOS_KERNEL_CORES > 1
        // The one-deep half is not witnessed above one kernel core. Queueing rather than
        // coalescing shows up only as a service that must not happen, and a service that does
        // not happen raises no event to order a later read after. Gating the driver does not
        // help: a queued latch is consumed at the next gate release and is indistinguishable
        // there from the redelivery of a fresh raise. Checked on every one-core preset.
        tap::partial("one-deep coalescing is a service that must NOT happen");
#else
        TAP_CHECK(others_parked());
        one_deep = (g_mask_serviced == 2);
#endif
        // Liveness: a fresh raise reaches the re-armed line, and the driver's loop ends.
        TAP_CHECK(kos_irq_raise(irq) == 0);
        TAP_CHECK(kos_sem_wait(g_done, STALL_TOLERANT_US) == 0); // phase A of the third service
        bool const live = (g_mask_serviced == 3);
        kos_sem_post(g_irq_ready);
        // Its last post, phase B, is banked by the time the join returns.
        TAP_CHECK(hold.joined());
        TAP_CHECK(kos_sem_wait(g_done, STALL_TOLERANT_US) == 0);

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
            kos_sem_wait(CH_READY, KOS_TIMEOUT_NONE); // main holds the masked window open
            irq.discard();          // retire anything coalesced onto the masked line
            irq.ack();
            kos_sem_post(CH_DONE);  // phase B: the line is ARMED again
        }
    }
    void t_irq_discard()
    {
        // A bad cap is refused at the same chokepoint as wait/ack, before any controller
        // write.
        TAP_CHECK(kos_irq_discard(KOS_CAP_NONE) == -KOS_EBADF);
        g_disc_serviced = 0;
        kos_cap_t irq = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        kos::thread::Handle drv;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_irq_ready) and hold.cap(&irq) and hold.cap(&note)
                  and hold.thread(&drv));
        TAP_ASK_LINE(&irq, .workers = 1, .sems = 1, .notifies = 1, .irq_line = DISCARD_LINE);
        TAP_CHECK(kos_sem_create(0, &g_irq_ready) == 0);
        note = notify_for_line(irq);
        TAP_CHECK(note != KOS_CAP_NONE);
        kos_irq_ack(irq); // arm the freshly-claimed (masked) line
        kos_cap_grant caps[] = {{g_done, CH_FULL},
                                {g_irq_ready, CH_FULL},
                                {irq, KOS_CAP_WAIT},
                                {note, CH_FULL}};
        drv = irq_spawn(discard_driver, nullptr, "discirq", IRQ_DRV_PRIO, caps, 4);
        TAP_CHECK(drv.valid());
        TAP_CHECK(hold.close(&note) == 0);
        TAP_CHECK(kos_sem_wait(g_irq_ready, STALL_TOLERANT_US) == 0);

        TAP_CHECK(kos_irq_raise(irq) == 0);
        // Phase A: serviced once, stopped before its discard, so masked from here
        TAP_CHECK(kos_sem_wait(g_done, STALL_TOLERANT_US) == 0);
        TAP_CHECK(kos_irq_raise(irq) == 0);
        TAP_CHECK(kos_irq_raise(irq) == 0);
        kos_sem_post(g_irq_ready); // release the window; the driver discards, then acks
        // Phase B is load-bearing here, not bookkeeping: a raise from main that beat the
        // discard would be retired by it, and the driver would then wait on a line nothing
        // will raise again.
        TAP_CHECK(kos_sem_wait(g_done, STALL_TOLERANT_US) == 0);
        bool retired = true;
#if KICKOS_KERNEL_CORES > 1
        // The retirement half is not witnessed above one kernel core. That the discard
        // dropped the coalesced latch shows up only as a second service that must not happen,
        // and a service that does not happen raises no event to order a later read after.
        // Checked on every one-core preset.
        tap::partial("a retired latch is a service that must NOT happen");
#else
        TAP_CHECK(others_parked());
        retired = (g_disc_serviced == 1);
#endif
        // Liveness, and this half holds on every core count: the discard must retire the
        // latch without wedging the line.
        TAP_CHECK(kos_irq_raise(irq) == 0);
        TAP_CHECK(kos_sem_wait(g_done, STALL_TOLERANT_US) == 0); // phase A of the second service
        bool const live = (g_disc_serviced == 2);
        kos_sem_post(g_irq_ready);
        // Its last post, phase B, is banked by the time the join returns.
        TAP_CHECK(hold.joined());
        TAP_CHECK(kos_sem_wait(g_done, STALL_TOLERANT_US) == 0);

        TAP_CHECK(retired);
        TAP_CHECK(live);
    }

    // --- Auto-rearm: wait; service with no explicit ack ------------------------
    // A notify_wait re-arms every signaller CHAINED on the object whose bit it accepts, so a
    // driver that never acks still receives every subsequent IRQ. Driver MUST run above main.
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
        g_autorearm_seen = 0;
        kos_cap_t irq = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        kos::thread::Handle drv;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_irq_ready) and hold.cap(&irq) and hold.cap(&note)
                  and hold.thread(&drv));
        TAP_ASK_LINE(&irq, .workers = 1, .sems = 1, .notifies = 1, .irq_line = AUTO_REARM_LINE);
        TAP_CHECK(kos_sem_create(0, &g_irq_ready) == 0);
        note = notify_for_line(irq);
        TAP_CHECK(note != KOS_CAP_NONE);
        kos_irq_ack(irq); // arm the freshly-claimed (masked) line before raising it
        kos_cap_grant caps[] = {{g_done, CH_FULL},
                                {g_irq_ready, CH_FULL},
                                {note, CH_FULL}};
        uint16_t const dest[] = {CH_DONE, CH_READY, CH_NOTE};
        drv = irq_spawn(autorearm_driver, nullptr, "autoirq", 15, caps, 3,
                        KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr,
                        0, /*authority=*/0, dest);
        TAP_CHECK(drv.valid());
        TAP_CHECK(hold.close(&note) == 0);
        // main keeps the line until the arm is over: it raises it, and the driver holds no
        // capability naming it, so closing it would drop the last reference, unchain the
        // signaller and take the line away mid-arm.
        TAP_CHECK(kos_sem_wait(g_irq_ready, STALL_TOLERANT_US) == 0);
        for (int i = 0; i < 3; i++)
        {
            TAP_CHECK(kos_irq_raise(irq) == 0);
            TAP_CHECK(kos_sem_wait(g_done, STALL_TOLERANT_US) == 0);
        }
        TAP_CHECK(hold.joined());
        TAP_CHECK(g_autorearm_seen == 3);
    }

    // --- No phantom wake in the ack;compute;wait shape -------------------------
    // After an explicit ack re-arms the line, exactly ONE raised event must yield exactly
    // ONE wait-return: the second wait BLOCKS. Setting needs_rearm in the ISR instead of on
    // wait-return unmasks early and phantom-posts. Every raise below must target an ARMED line,
    // and the mid-compute one must land between the ack and the next wait.
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
        kos_sem_post(CH_DONE); // acked; main raises the one mid-compute event
        kos_sem_wait(CH_READY, KOS_TIMEOUT_NONE); // main holds the compute until it has raised
        note.wait(NOTE_ALL);
        g_phantom_seen++;
        kos_sem_post(CH_DONE);
        note.wait(NOTE_ALL);   // MUST block: only one event was raised, no phantom
        g_phantom_seen++;      // reached only on a phantom wake (the bug)
        kos_sem_post(CH_DONE);
    }
    void t_irq_phantom()
    {
        g_phantom_seen = 0;
        kos_cap_t irq = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        kos::thread::Handle drv;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_irq_ready) and hold.cap(&irq) and hold.cap(&note)
                  and hold.thread(&drv));
        TAP_ASK_LINE(&irq, .workers = 1, .sems = 1, .notifies = 1, .irq_line = PHANTOM_LINE);
        TAP_CHECK(kos_sem_create(0, &g_irq_ready) == 0);
        note = notify_for_line(irq);
        TAP_CHECK(note != KOS_CAP_NONE);
        kos_irq_ack(irq); // arm: every raise below must target an ARMED line
        kos_cap_grant caps[] = {{g_done, CH_FULL},
                                {g_irq_ready, CH_FULL},
                                {irq, KOS_CAP_WAIT},
                                {note, CH_FULL}};
        drv = irq_spawn(phantom_driver, nullptr, "phantirq", IRQ_DRV_PRIO, caps, 4);
        TAP_CHECK(drv.valid());
        TAP_CHECK(hold.close(&note) == 0);
        TAP_CHECK(kos_sem_wait(g_irq_ready, STALL_TOLERANT_US) == 0);

        TAP_CHECK(kos_irq_raise(irq) == 0);
        TAP_CHECK(kos_sem_wait(g_done, STALL_TOLERANT_US) == 0);

        TAP_CHECK(kos_irq_raise(irq) == 0); // the one mid-compute event, on the armed line
        kos_sem_post(g_irq_ready);
        TAP_CHECK(kos_sem_wait(g_done, STALL_TOLERANT_US) == 0);
        TAP_CHECK(g_phantom_seen == 1);

        TAP_CHECK(others_parked());
        TAP_CHECK(g_phantom_seen == 1);

        // Wait is live (blocked, not lost) and the line re-armed itself.
        TAP_CHECK(kos_irq_raise(irq) == 0);
        TAP_CHECK(kos_sem_wait(g_done, STALL_TOLERANT_US) == 0);
        TAP_CHECK(g_phantom_seen == 2);
        TAP_CHECK(hold.joined());
    }

#endif // KICKOS_ENABLE_SELFTEST (tier-1 IRQ + mask)

    // --- Semaphore destroy: freelist reuse + generation-tagged handles ---------
    void t_sem_destroy()
    {
        TAP_ASK(.sems = 1);
        kos_cap_t h = KOS_CAP_NONE;
        kos_cap_t h2 = KOS_CAP_NONE;
        kos_cap_t hmax = KOS_CAP_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&h) and hold.cap(&h2) and hold.cap(&hmax));
        TAP_CHECK(kos_sem_create(0, &h) == 0);
        TAP_CHECK(kos_sem_destroy(h) == 0);
        kos_cap_t const stale = h;
        h = KOS_CAP_NONE;
        TAP_CHECK(kos_sem_destroy(stale) == -KOS_EBADF);
        // A reused slot carries a fresh generation.
        TAP_CHECK(kos_sem_create(0, &h2) == 0 and h2 != stale);
        TAP_CHECK(kos_sem_destroy(h2) == 0);
        h2 = KOS_CAP_NONE;
        // Malformed caps must fail with the SPECIFIC code -KOS_EBADF, not any negative.
        // wait/post share the same cap_resolve_e chokepoint.
        TAP_CHECK(kos_handle_close(KOS_CAP_NONE) == -KOS_EBADF);
        TAP_CHECK(kos_handle_close(0x7fffffff) == -KOS_EBADF);
        TAP_CHECK(kos_handle_close(0x00ffffff) == -KOS_EBADF);
        // The count is bounded at both ends: birth outside [0, KOS_SEM_COUNT_MAX] is
        // refused, and a post at the ceiling with no waiter is refused, not overflowed.
        kos_cap_t bad = KOS_CAP_NONE;
        TAP_CHECK(kos_sem_create(-1, &bad) == -KOS_EINVAL and bad == KOS_CAP_NONE);
        TAP_CHECK(kos_sem_create(KOS_SEM_COUNT_MAX, &hmax) == 0
                  and kos_sem_post(hmax) == -KOS_EOVERFLOW
                  and hold.close(&hmax) == 0);
    }

    // --- Refcounted close of a DELEGATED sem: object survives while a co-holder is
    // parked; the last close frees it.
    kos_cap_t g_dsem = KOS_CAP_NONE;
    void destroy_waiter(void*) // caps: g_dsem@1
    {
        kos_sem_wait(1, KOS_TIMEOUT_NONE);
    }
    void destroy_poster(void*) // caps: g_dsem@1
    {
        kos_sem_post(1);
    }
    void t_sem_destroy_busy()
    {
        TAP_ASK(.workers = 2, .sems = 1);
        kos::thread::Handle w;
        kos::thread::Handle p;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_dsem) and hold.thread(&w) and hold.thread(&p));
        TAP_CHECK(kos_sem_create(0, &g_dsem) == 0);
        kos_cap_grant caps[] = {{g_dsem, CH_FULL}};
        w = irq_spawn(destroy_waiter, nullptr, "dwaiter", TAP_PRIO_PARKS, caps, 1);
        p = irq_spawn(destroy_poster, nullptr, "dposter", TAP_PRIO_AFTER, caps, 1);
        TAP_CHECK(w.valid() and p.valid());
        kos_yield();
        // Close MAIN's cap while the waiter is parked and before the poster posts: refs
        // 3 -> 2, so the object must survive. A freed object or wait queue would leave the
        // poster's post unable to wake the waiter, and the join would time out.
        TAP_CHECK(hold.close(&g_dsem) == 0);
        TAP_CHECK(hold.joined());
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

    // --- Timed semaphore wait ---------------------------------------------------
    constexpr uint32_t SEM_TIMEOUT_US = 2000;
    void sem_timed_poster(void*) // caps: S(SIGNAL)@1
    {
        (void)kos_sem_post(KOS_SPAWN_DELEGATED_CAP0);
    }
    void t_sem_timed()
    {
        TAP_ASK(.workers = 1, .sems = 1);
        kos_cap_t s = KOS_CAP_NONE;
        kos::thread::Handle p;
        ArmHold hold;
        TAP_HOLD(hold.cap(&s) and hold.thread(&p));
        TAP_CHECK(kos_sem_create(0, &s) == 0);
        TAP_CHECK(kos_sem_wait(s, 0) == -KOS_ETIMEDOUT);
        uint64_t const t0 = kos_clock_now();
        int const rc = kos_sem_wait(s, SEM_TIMEOUT_US);
        uint64_t const waited_us = (kos_clock_now() - t0) / 1000u;
        TAP_CHECK(rc == -KOS_ETIMEDOUT);
        TAP_CHECK(waited_us >= SEM_TIMEOUT_US);
        TAP_CHECK(waited_us < STALL_TOLERANT_US);
        // The expired waiter left the queue: the post is banked, and banked once.
        TAP_CHECK(kos_sem_post(s) == 0);
        TAP_CHECK(kos_sem_wait(s, 0) == 0);
        TAP_CHECK(kos_sem_wait(s, 0) == -KOS_ETIMEDOUT);
        kos_cap_grant const caps[] = {{s, KOS_CAP_SIGNAL}};
        p = irq_spawn(sem_timed_poster, nullptr, "stpost", TAP_PRIO_AFTER, caps, 1);
        TAP_CHECK(p.valid());
        TAP_CHECK(kos_sem_wait(s, STALL_TOLERANT_US) == 0);
        TAP_CHECK(hold.joined());
        TAP_CHECK(kos_sem_wait(s, 0) == -KOS_ETIMEDOUT);
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
    }
    void t_mutex_basic()
    {
        TAP_ASK(.workers = 3, .mutexes = 1);
        kos_cap_t m = KOS_CAP_NONE;
        kos::thread::Handle a;
        kos::thread::Handle b;
        kos::thread::Handle c;
        ArmHold hold;
        TAP_HOLD(hold.cap(&m) and hold.thread(&a) and hold.thread(&b) and hold.thread(&c));
        TAP_CHECK(kos_mutex_create(&m) == 0);
        g_mtx_shared = 0;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {m, CH_MTX}};
        a = kos::thread::create_caps(mtx_basic_worker, nullptr, "mbA", 10, caps, 2);
        b = kos::thread::create_caps(mtx_basic_worker, nullptr, "mbB", 10, caps, 2);
        c = kos::thread::create_caps(mtx_basic_worker, nullptr, "mbC", 10, caps, 2);
        TAP_CHECK(a.valid() and b.valid() and c.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(hold.close(&m) == 0);
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
        kos_sem_post(5);
        log_put('u');
        kos_mutex_unlock(3);
        log_put('z');        // reached only after med (12) has run -> proves revert
    }
    void pi_high(void*) // caps: done@1, lock@2, mutex@3, gate@4, go@5
    {
        stage_wait(4);
        kos_sem_wait(5, KOS_TIMEOUT_NONE);
        log_put('h');
        kos_sem_post(5);
        kos_mutex_lock(3); // low holds it -> block, boost low to 20
        log_put('H');
        kos_mutex_unlock(3);
    }
    void pi_med(void*) // caps: done@1, lock@2, gate@3, go@4
    {
        stage_wait(3);
        kos_sem_wait(4, KOS_TIMEOUT_NONE);
        log_put('m');
    }
    void t_mutex_pi()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        TAP_ASK(.workers = 3, .sems = 2, .mutexes = 1);
        log_reset();
        kos_cap_t m = KOS_CAP_NONE;
        kos::thread::Handle lo;
        kos::thread::Handle hi;
        kos::thread::Handle md;
        ArmHold hold;
        TAP_HOLD(hold.cap(&m) and hold.cap(&g_gate) and hold.cap(&g_pi_go) and hold.thread(&lo)
                  and hold.thread(&hi) and hold.thread(&md));
        TAP_CHECK(kos_mutex_create(&m) == 0);
        TAP_CHECK(kos_sem_create(0, &g_gate) == 0);
        TAP_CHECK(kos_sem_create(0, &g_pi_go) == 0);
        kos_cap_grant lcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {m, CH_MTX},
                                 {g_gate, CH_FULL}, {g_pi_go, CH_FULL}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_gate, CH_FULL},
                                 {g_pi_go, CH_FULL}};
        lo = kos::thread::create_caps(pi_low, nullptr, "piLo", 8, lcaps, 5);
        hi = kos::thread::create_caps(pi_high, nullptr, "piHi", 20, lcaps, 5);
        md = kos::thread::create_caps(pi_med, nullptr, "piMd", 12, mcaps, 4);
        TAP_CHECK(lo.valid() and hi.valid() and md.valid());
        stage_release();
        TAP_CHECK(hold.joined());
        TAP_CHECK(hold.close(&m) == 0);
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
        kos_sem_wait(5, KOS_TIMEOUT_NONE);
        kos_sem_post(6);          // only now may A block on M1
        mtx_spin(g_mtx_unit * 8); // A blocks on M1 inside this
        log_put('e');
        kos_mutex_unlock(3);
        log_put('C');
    }
    void ch_b(void*) // caps: done@1, lock@2, M1@3, M2@4, up@5
    {
        kos_sem_wait(5, KOS_TIMEOUT_NONE);
        kos_mutex_lock(3); // M1 (before A tries it)
        log_put('b');
        kos_sem_post(5);
        kos_mutex_lock(4); // M2: C holds it -> block, boost C to 10, which resumes C's wait
        kos_mutex_unlock(4);
        kos_mutex_unlock(3);
    }
    void ch_a(void*) // caps: done@1, lock@2, M1@3, on@4
    {
        kos_sem_wait(4, KOS_TIMEOUT_NONE);
        kos_sem_post(4);
        kos_mutex_lock(3); // M1: B holds it AND waits on M2 -> boost B to 20, then C to 20
        kos_mutex_unlock(3);
    }
    void ch_d(void*) // caps: done@1, lock@2, on@3
    {
        // Index 3, not the 4 A posts on: holding no mutex cap shifts the same semaphore one
        // slot down. Waiting on the wrong index returns at once and 'd' precedes 'e'.
        kos_sem_wait(3, KOS_TIMEOUT_NONE);
        log_put('d');
    }
    void t_mutex_chain()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        TAP_ASK(.workers = 4, .sems = 3, .mutexes = 2);
        log_reset();
        g_mtx_unit = mtx_time_unit();
        kos_cap_t m1 = KOS_CAP_NONE;
        kos_cap_t m2 = KOS_CAP_NONE;
        kos::thread::Handle c;
        kos::thread::Handle b;
        kos::thread::Handle a;
        kos::thread::Handle d;
        ArmHold hold;
        TAP_HOLD(hold.cap(&m1) and hold.cap(&m2) and hold.cap(&g_gate) and hold.cap(&g_ch_up)
                  and hold.cap(&g_ch_on));
        TAP_HOLD(hold.thread(&c) and hold.thread(&b) and hold.thread(&a) and hold.thread(&d));
        TAP_CHECK(kos_mutex_create(&m1) == 0);
        TAP_CHECK(kos_mutex_create(&m2) == 0);
        TAP_CHECK(kos_sem_create(0, &g_gate) == 0);
        TAP_CHECK(kos_sem_create(0, &g_ch_up) == 0);
        TAP_CHECK(kos_sem_create(0, &g_ch_on) == 0);
        // Six grants, exactly KICKOS_MAX_SPAWN_GRANTS. g_done stays at index 1, unposted, so
        // log_put finds the log lock at CH_LOCK.
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {m2, CH_MTX},
                                 {g_gate, CH_FULL}, {g_ch_up, CH_FULL}, {g_ch_on, CH_FULL}};
        kos_cap_grant bcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL},
                                 {m1, CH_MTX}, {m2, CH_MTX}, {g_ch_up, CH_FULL}};
        kos_cap_grant acaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {m1, CH_MTX},
                                 {g_ch_on, CH_FULL}};
        kos_cap_grant dcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ch_on, CH_FULL}};
        c = kos::thread::create_caps(ch_c, nullptr, "chC", 5, ccaps, 6);
        b = kos::thread::create_caps(ch_b, nullptr, "chB", 10, bcaps, 5);
        a = kos::thread::create_caps(ch_a, nullptr, "chA", 20, acaps, 4);
        d = kos::thread::create_caps(ch_d, nullptr, "chD", 15, dcaps, 3);
        TAP_CHECK(c.valid() and b.valid() and a.valid() and d.valid());
        stage_release();
        TAP_CHECK(hold.joined());
        TAP_CHECK(count('c') == 1 and count('e') == 1 and count('d') == 1
                  and count('b') == 1 and count('C') == 1);
        TAP_CHECK(nth('b', 1) < nth('e', 1)); // chain formed (B took M1 before C released M2)
        TAP_CHECK(nth('e', 1) < nth('d', 1)); // chain boost: C ran above med across two hops
    }

    // --- Owner dies holding the mutex: waiter gets OWNER_DIED (H7) --------------
    // The owner must exit WHILE still holding, with the waiter already queued: that is what
    // makes cap_teardown force-unlock as a HANDOFF and the waiter's lock() return OWNER_DIED.
    // An owner that exits first force-unlocks with nobody queued, and the waiter's plain 0 is
    // indistinguishable from the defect.
    int g_od_result = -99;
    void od_owner(void*) // caps: mutex@1, holds@2
    {
        kos_mutex_lock(1);
        kos_sem_post(2);
        kos_exit(0);     // exits still owning -> force-unlock
    }
    void od_waiter(void*) // caps: mutex@1, holds@2
    {
        kos_sem_wait(2, KOS_TIMEOUT_NONE);
        g_od_result = kos_mutex_lock(1);
        // -KOS_EOWNERDEAD is a HELD acquire: unlock it too, or the robust mutex is stranded.
        // A plain `>= 0` test would skip this, since owner-died is a NEGATIVE code.
        if (g_od_result == 0 or g_od_result == -KOS_EOWNERDEAD)
        {
            kos_mutex_unlock(1);
        }
    }
    void t_mutex_owner_died()
    {
        TAP_ASK(.workers = 2, .sems = 1, .mutexes = 1);
        g_od_result = -99;
        kos_cap_t m = KOS_CAP_NONE;
        kos_cap_t holds = KOS_CAP_NONE;
        kos::thread::Handle ow;
        kos::thread::Handle wt;
        ArmHold hold;
        TAP_HOLD(hold.cap(&m) and hold.cap(&holds) and hold.thread(&ow) and hold.thread(&wt));
        TAP_CHECK(kos_mutex_create(&m) == 0);
        TAP_CHECK(kos_sem_create(0, &holds) == 0);
        kos_cap_grant caps[] = {{m, CH_MTX}, {holds, CH_FULL}};
        ow = irq_spawn(od_owner, nullptr, "odOwn", 8, caps, 2);
        wt = irq_spawn(od_waiter, nullptr, "odWt", 12, caps, 2);
        TAP_CHECK(ow.valid() and wt.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(g_od_result == -KOS_EOWNERDEAD);
        TAP_CHECK(hold.close(&m) == 0);
    }

    // --- Deadlock refused -KOS_EDEADLK (H6): self-lock + a two-mutex wait cycle -
    int g_cyc_rb = -99;
    void cyc_a(void*) // caps: done@1, M1@2, M2@3, have1@4, goA@5
    {
        kos_mutex_lock(2); // M1
        kos_sem_post(4);   // have1
        kos_sem_wait(5, KOS_TIMEOUT_NONE);   // goA
        int r = kos_mutex_lock(3); // M2: B holds -> block; later handed off (r==0)
        if (r == 0)
        {
            kos_mutex_unlock(3);
        }
        kos_mutex_unlock(2);
    }
    void cyc_b(void*) // caps: done@1, M2@2, M1@3, have2@4, goB@5
    {
        kos_mutex_lock(2); // M2
        kos_sem_post(4);   // have2
        kos_sem_wait(5, KOS_TIMEOUT_NONE);   // goB
        g_cyc_rb = kos_mutex_lock(3); // M1: closes the cycle -> refused
        if (g_cyc_rb == 0)
        {
            kos_mutex_unlock(3);
        }
        kos_mutex_unlock(2); // release M2 -> hands it to A
    }

    void t_mutex_deadlock()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        TAP_ASK(.workers = 2, .sems = 4, .mutexes = 2);
        kos_cap_t self = KOS_CAP_NONE;
        kos_cap_t m1 = KOS_CAP_NONE;
        kos_cap_t m2 = KOS_CAP_NONE;
        kos_cap_t have1 = KOS_CAP_NONE;
        kos_cap_t have2 = KOS_CAP_NONE;
        kos_cap_t goA = KOS_CAP_NONE;
        kos_cap_t goB = KOS_CAP_NONE;
        kos::thread::Handle a;
        kos::thread::Handle b;
        ArmHold hold;
        TAP_HOLD(hold.owned(&self) and hold.cap(&m1) and hold.cap(&m2) and hold.cap(&have1)
                  and hold.cap(&have2) and hold.cap(&goA) and hold.cap(&goB));
        TAP_HOLD(hold.thread(&a) and hold.thread(&b));
        // Self-deadlock: a recursive lock is refused (-KOS_EDEADLK), not parked, and leaves
        // the mutex holdable/releasable normally.
        TAP_CHECK(kos_mutex_create(&self) == 0);
        TAP_CHECK(kos_mutex_lock(self) == 0);
        TAP_CHECK(kos_mutex_lock(self) == -KOS_EDEADLK);
        TAP_CHECK(kos_mutex_unlock(self) == 0);
        TAP_CHECK(hold.close(&self) == 0);

        // Cross-thread cycle: A owns M1 + waits M2; B owns M2 + tries M1 -> -KOS_EDEADLK.
        g_cyc_rb = -99;
        TAP_CHECK(kos_mutex_create(&m1) == 0);
        TAP_CHECK(kos_mutex_create(&m2) == 0);
        TAP_CHECK(kos_sem_create(0, &have1) == 0);
        TAP_CHECK(kos_sem_create(0, &have2) == 0);
        TAP_CHECK(kos_sem_create(0, &goA) == 0);
        TAP_CHECK(kos_sem_create(0, &goB) == 0);
        kos_cap_grant acaps[] = {{g_done, CH_FULL}, {m1, CH_MTX}, {m2, CH_MTX},
                                 {have1, CH_FULL}, {goA, CH_FULL}};
        kos_cap_grant bcaps[] = {{g_done, CH_FULL}, {m2, CH_MTX}, {m1, CH_MTX},
                                 {have2, CH_FULL}, {goB, CH_FULL}};
        a = kos::thread::create_caps(cyc_a, nullptr, "cycA", 10, acaps, 5);
        b = kos::thread::create_caps(cyc_b, nullptr, "cycB", 10, bcaps, 5);
        TAP_CHECK(a.valid() and b.valid());
        TAP_CHECK(kos_sem_wait(have1, STALL_TOLERANT_US) == 0); // A owns M1
        TAP_CHECK(kos_sem_wait(have2, STALL_TOLERANT_US) == 0); // B owns M2
        kos_sem_post(goA);   // A tries M2 -> blocks (B owns it)
        kos_sem_post(goB);   // B tries M1 -> would cycle -> -KOS_EDEADLK, not parked
        TAP_CHECK(hold.joined());
        TAP_CHECK(g_cyc_rb == -KOS_EDEADLK);
        TAP_CHECK(hold.close(&m1) == 0 and hold.close(&m2) == 0);
    }

    // --- ArmHold releases every kind it holds -----------------------------------
    int g_ah_locked = -99;
    void ah_parked(void*) // caps: gate@1
    {
        kos_sem_wait(1, KOS_TIMEOUT_NONE);
    }
    void ah_locker(void*) // caps: mutex@1
    {
        g_ah_locked = kos_mutex_lock(1);
        if (g_ah_locked == 0)
        {
            kos_mutex_unlock(1);
        }
    }
    void ah_last(void*) {}
    // Its join times out, so the parked thread is slain.
    constexpr uint32_t AH_PARK_US = 20000;
    void t_arm_hold()
    {
        TAP_ASK(.workers = 3, .tasks = 1, .sems = 1, .mutexes = 1, .notifies = 1);
        g_ah_locked = -99;
#if KICKOS_HAVE_ASPACE
        uint32_t frames0 = 0;
        uint32_t ranges0 = 0;
        TAP_CHECK(kos_mem_count(KOS_MEM_FRAMES_FREE, &frames0) == 0
                  and kos_mem_count(KOS_MEM_RANGES_FREE, &ranges0) == 0);
        kos_cap_t frame = KOS_CAP_NONE;
        kos_cap_t space = KOS_CAP_NONE;
        uintptr_t va = 0;
#endif
        kos_cap_t gate = KOS_CAP_NONE;
        kos_cap_t m = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        kos_task_t task = KOS_TASK_NONE;
        kos_task_t held_task = KOS_TASK_NONE;
        kos::thread::Handle parked;
        kos::thread::Handle locker;
        kos::thread::Handle last;
        {
            ArmHold hold;
            TAP_HOLD(hold.cap(&gate) and hold.owned(&m) and hold.bound(&note)
                      and hold.task(&task) and hold.thread(&locker) and hold.thread(&last));
#if KICKOS_HAVE_ASPACE
            TAP_HOLD(hold.mapped(&frame, &space, &va));
            TAP_CHECK(kos_frame_create(ASPACE_GRANULE, &frame) == 0
                      and kos_aspace_self(&space) == 0);
            TAP_CHECK(kos_frame_map(frame, space, &va, 0) == 0);
#endif
            TAP_CHECK(kos_sem_create(0, &gate) == 0);
            TAP_CHECK(kos_mutex_create(&m) == 0 and kos_mutex_lock(m) == 0);
            TAP_CHECK(kos_notify_create(&note) == 0 and kos_notify_bind(note) == 0);
            TAP_CHECK(kos_task_create(nullptr, 0, 0, &task) == 0);
            held_task = task;
            kos_cap_grant gcaps[] = {{gate, KOS_CAP_WAIT}};
            auto const member = kos::thread::create_caps(ah_parked, nullptr, "ahM", 10, gcaps, 1,
                                                         KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                                         0, nullptr, task);
            TAP_CHECK(member.valid());
            {
                ArmHold quick(AH_PARK_US);
                TAP_HOLD(quick.thread(&parked));
                parked = kos::thread::create_caps(ah_parked, nullptr, "ahP", 10, gcaps, 1);
                TAP_CHECK(parked.valid());
            }
            TAP_CHECK(parked.join(0) != -KOS_ETIMEDOUT);
            kos_cap_grant mcaps[] = {{m, CH_MTX}};
            locker = kos::thread::create_caps(ah_locker, nullptr, "ahK", 10, mcaps, 1);
            TAP_CHECK(locker.valid());
            // Below main, so it has run only if the hold joined it.
            last = irq_spawn(ah_last, nullptr, "ahL", TAP_PRIO_AFTER, nullptr, 0);
            TAP_CHECK(last.valid());
        }
        TAP_CHECK(gate == KOS_CAP_NONE and m == KOS_CAP_NONE and note == KOS_CAP_NONE);
        TAP_CHECK(task == KOS_TASK_NONE and kos_task_state(held_task) == -KOS_EBADF);
        TAP_CHECK(g_ah_locked == 0);
        TAP_CHECK(last.join(0) == 0);
#if KICKOS_HAVE_ASPACE
        uint32_t frames1 = 0;
        uint32_t ranges1 = 0;
        TAP_CHECK(kos_mem_count(KOS_MEM_FRAMES_FREE, &frames1) == 0
                  and kos_mem_count(KOS_MEM_RANGES_FREE, &ranges1) == 0);
        TAP_CHECK(frame == KOS_CAP_NONE and space == KOS_CAP_NONE);
        TAP_CHECK(frames1 == frames0 and ranges1 == ranges0);
#endif
        kos_cap_t again = KOS_CAP_NONE;
        TAP_CHECK(kos_notify_create(&again) == 0);
        int const rebound = kos_notify_bind(again);
        if (rebound == 0)
        {
            (void)kos_notify_unbind(again);
        }
        (void)kos_handle_close(again);
        TAP_CHECK(rebound == 0);
    }

    // --- Closing a mutex you own is refused -------------------------------------
    void t_mutex_close_owned()
    {
        TAP_ASK(.mutexes = 1);
        kos_cap_t m = KOS_CAP_NONE;
        ArmHold hold;
        TAP_HOLD(hold.owned(&m));
        TAP_CHECK(kos_mutex_create(&m) == 0);
        TAP_CHECK(kos_mutex_lock(m) == 0);
        TAP_CHECK(kos_handle_close(m) == -KOS_EBUSY);
        TAP_CHECK(kos_mutex_unlock(m) == 0);
        TAP_CHECK(hold.close(&m) == 0);
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
        kos_sem_post(6);
        kos_mutex_unlock(4); // release M2; H on M1 must keep B boosted
        log_put('x');
        kos_mutex_unlock(3); // release M1 -> hand to H, B drops to base
    }
    void mh_h(void*) // caps: done@1, lock@2, M1@3, gate@4, go@5
    {
        stage_wait(4);
        kos_sem_wait(5, KOS_TIMEOUT_NONE);
        log_put('h');
        kos_sem_post(5);
        kos_mutex_lock(3); // M1: B holds -> block, boost B to 20
        log_put('H');
        kos_mutex_unlock(3);
    }
    void mh_d(void*) // caps: done@1, lock@2, gate@3, go@4
    {
        stage_wait(3);
        kos_sem_wait(4, KOS_TIMEOUT_NONE);
        log_put('d');
    }
    void t_mutex_multi_held()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        TAP_ASK(.workers = 3, .sems = 2, .mutexes = 2);
        log_reset();
        kos_cap_t m1 = KOS_CAP_NONE;
        kos_cap_t m2 = KOS_CAP_NONE;
        kos::thread::Handle b;
        kos::thread::Handle h;
        kos::thread::Handle d;
        ArmHold hold;
        TAP_HOLD(hold.cap(&m1) and hold.cap(&m2) and hold.cap(&g_gate) and hold.cap(&g_mh_go)
                  and hold.thread(&b) and hold.thread(&h) and hold.thread(&d));
        TAP_CHECK(kos_mutex_create(&m1) == 0);
        TAP_CHECK(kos_mutex_create(&m2) == 0);
        TAP_CHECK(kos_sem_create(0, &g_gate) == 0);
        TAP_CHECK(kos_sem_create(0, &g_mh_go) == 0);
        kos_cap_grant bcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL},
                                 {m1, CH_MTX}, {m2, CH_MTX}, {g_gate, CH_FULL},
                                 {g_mh_go, CH_FULL}};
        kos_cap_grant hcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {m1, CH_MTX},
                                 {g_gate, CH_FULL}, {g_mh_go, CH_FULL}};
        kos_cap_grant dcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_gate, CH_FULL},
                                 {g_mh_go, CH_FULL}};
        b = kos::thread::create_caps(mh_b, nullptr, "mhB", 6, bcaps, 6);
        h = kos::thread::create_caps(mh_h, nullptr, "mhH", 20, hcaps, 5);
        d = kos::thread::create_caps(mh_d, nullptr, "mhD", 12, dcaps, 4);
        TAP_CHECK(b.valid() and h.valid() and d.valid());
        stage_release();
        TAP_CHECK(hold.joined());
        TAP_CHECK(hold.close(&m1) == 0 and hold.close(&m2) == 0);
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
    }
    void t_mutex_unlock_errors()
    {
        TAP_ASK(.workers = 1, .mutexes = 1);
        kos_cap_t m = KOS_CAP_NONE;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.owned(&m) and hold.thread(&w));
        TAP_CHECK(kos_mutex_create(&m) == 0);
        TAP_CHECK(kos_mutex_unlock(m) == -KOS_EPERM); // unlocked: caller is not the (null) owner
        TAP_CHECK(kos_mutex_lock(m) == 0);
        g_nonowner_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {m, CH_MTX}};
        w = kos::thread::create_caps(nonowner_unlock, nullptr, "nonown", 10, caps, 2);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(g_nonowner_rc == -KOS_EPERM);
        TAP_CHECK(kos_mutex_unlock(m) == 0);
        TAP_CHECK(hold.close(&m) == 0);
    }

    // --- Owner dies holding with NO waiter: m->owner cleared, re-lockable -------
    void od_solo_owner(void*) // caps: mutex@1
    {
        kos_mutex_lock(1);
        kos_exit(0); // exits owning, no waiter -> force-unlock nulls m->owner
    }
    void t_mutex_owner_died_nowaiter()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        TAP_ASK(.workers = 1, .mutexes = 1);
        kos_cap_t m = KOS_CAP_NONE;
        kos::thread::Handle ow;
        ArmHold hold;
        TAP_HOLD(hold.owned(&m) and hold.thread(&ow));
        TAP_CHECK(kos_mutex_create(&m) == 0);
        kos_cap_grant ocaps[] = {{m, CH_MTX}};
        ow = kos::thread::create_caps(od_solo_owner, nullptr, "odSolo", 8, ocaps, 1);
        TAP_CHECK(ow.valid());
        TAP_CHECK(hold.joined()); // the owner acquired, then exited owning
        // If force-unlock did not null m->owner, this lock would block forever on a
        // dead owner. It must acquire cleanly (fresh, uncontended -> 0).
        TAP_CHECK(kos_mutex_lock(m) == 0);
        TAP_CHECK(kos_mutex_unlock(m) == 0);
        TAP_CHECK(hold.close(&m) == 0);
    }

    // --- Delegated-mutex refcount: child closes its cap, parent still locks ------
    void deleg_closer(void*) // caps: done@1, mutex@2
    {
        kos_handle_close(2);   // drop the child's delegated cap (refs 2 -> 1)
    }
    void t_mutex_deleg_refcount()
    {
        TAP_ASK(.workers = 1, .mutexes = 1);
        kos_cap_t m = KOS_CAP_NONE; // refs = 1 (main)
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.owned(&m) and hold.thread(&w));
        TAP_CHECK(kos_mutex_create(&m) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {m, CH_MTX}};  // refs -> 2
        w = kos::thread::create_caps(deleg_closer, nullptr, "delcl", 10, caps, 2);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        // Child closed its cap (and exited): the object must survive on main's cap.
        TAP_CHECK(kos_mutex_lock(m) == 0);
        TAP_CHECK(kos_mutex_unlock(m) == 0);
        TAP_CHECK(hold.close(&m) == 0);
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
    }
    void sp_peer(void*) // caps: done@1, lock@2, gate@3, go@4
    {
        stage_wait(3);
        kos_sem_wait(4, KOS_TIMEOUT_NONE);
        log_put('p');
    }
    void t_prio_self_raise_lower()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        TAP_ASK(.workers = 2, .sems = 2);
        log_reset();
        g_sp_raise = -99;
        g_sp_lower = -99;
        kos::thread::Handle sp;
        kos::thread::Handle pp;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_gate) and hold.cap(&g_sp_go) and hold.thread(&sp)
                  and hold.thread(&pp));
        TAP_CHECK(kos_sem_create(0, &g_gate) == 0);
        TAP_CHECK(kos_sem_create(0, &g_sp_go) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_gate, CH_FULL},
                                {g_sp_go, CH_FULL}};
        sp = kos::thread::create_caps(sp_self, nullptr, "spSelf", SP_LOW, caps, 4);
        pp = kos::thread::create_caps(sp_peer, nullptr, "spPeer", SP_PEER, caps, 4);
        TAP_CHECK(sp.valid() and pp.valid());
        stage_release();
        TAP_CHECK(hold.joined());
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
        kos_sem_post(5);
        g_pb_set = kos_thread_set_priority(PB_BASE);
        log_put('u');
        kos_mutex_unlock(3);
        log_put('z');
    }
    void pb_high(void*) // caps: done@1, lock@2, mutex@3, gate@4, go@5
    {
        stage_wait(4);
        kos_sem_wait(5, KOS_TIMEOUT_NONE);
        log_put('h');
        // M takes the first token and K, still short of its own wait, the banked second.
        kos_sem_post(5);
        kos_sem_post(5);
        kos_mutex_lock(3); // L holds it -> block, boost L to 20
        log_put('H');
        kos_mutex_unlock(3);
    }
    void pb_waiter(void* arg) // caps: done@1, lock@2, gate@3, go@4
    {
        stage_wait(3);
        kos_sem_wait(4, KOS_TIMEOUT_NONE);
        log_put(arg_char(arg));
    }
    void t_prio_self_boosted()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        TAP_ASK(.workers = 4, .sems = 2, .mutexes = 1);
        log_reset();
        g_pb_set = -99;
        kos_cap_t m = KOS_CAP_NONE;
        kos::thread::Handle lo;
        kos::thread::Handle hi;
        kos::thread::Handle md;
        kos::thread::Handle kw;
        ArmHold hold;
        TAP_HOLD(hold.cap(&m) and hold.cap(&g_gate) and hold.cap(&g_pb_go) and hold.thread(&lo)
                  and hold.thread(&hi) and hold.thread(&md) and hold.thread(&kw));
        TAP_CHECK(kos_mutex_create(&m) == 0);
        TAP_CHECK(kos_sem_create(0, &g_gate) == 0);
        TAP_CHECK(kos_sem_create(0, &g_pb_go) == 0);
        kos_cap_grant lcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {m, CH_MTX},
                                 {g_gate, CH_FULL}, {g_pb_go, CH_FULL}};
        kos_cap_grant wcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_gate, CH_FULL},
                                 {g_pb_go, CH_FULL}};
        void* const med_tag = reinterpret_cast<void*>(static_cast<uintptr_t>('m'));
        void* const k_tag = reinterpret_cast<void*>(static_cast<uintptr_t>('k'));
        lo = kos::thread::create_caps(pb_low, nullptr, "pbLo", PB_LOW, lcaps, 5);
        hi = kos::thread::create_caps(pb_high, nullptr, "pbHi", PB_HIGH, lcaps, 5);
        md = kos::thread::create_caps(pb_waiter, med_tag, "pbMd", PB_MED, wcaps, 4);
        kw = kos::thread::create_caps(pb_waiter, k_tag, "pbK", PB_K, wcaps, 4);
        TAP_CHECK(lo.valid() and hi.valid() and md.valid() and kw.valid());
        stage_release();
        TAP_CHECK(hold.joined());
        TAP_CHECK(hold.close(&m) == 0);
        TAP_CHECK(g_pb_set == 0);
        TAP_CHECK(count('l') == 1 and count('h') == 1 and count('u') == 1 and count('H') == 1
                  and count('m') == 1 and count('k') == 1 and count('z') == 1);
        TAP_CHECK(nth('h', 1) < nth('u', 1)); // H waited on the mutex before the lowering
        TAP_CHECK(nth('u', 1) < nth('m', 1)); // BOOST KEPT: lowering the base left L at 20
        TAP_CHECK(nth('u', 1) < nth('H', 1));
        TAP_CHECK(nth('m', 1) < nth('z', 1));
        TAP_CHECK(nth('k', 1) < nth('z', 1)); // NEW BASE: L fell below K once unlocked
    }

    // --- Per-grant destination indices: the refusals ---------------------------
    // A bad placement list must be REFUSED before anything is built.
    // Both spawns below must fail, so this body is deliberately never reached.
    void capdest_never_runs(void*) { kos_exit(0); }

    // Posts from a NON-default index. Ignoring the placement puts the semaphore at index 1, the
    // post at index 3 then finds nothing, and the semaphore stays one short of its ceiling.
    void capdest_probe(void*) { kos_sem_post(CH_IRQ); }

    void t_cap_dest()
    {
        TAP_ASK(.workers = 1);
        kos_cap_t sem = KOS_CAP_NONE;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.cap(&sem) and hold.thread(&w));
        TAP_CHECK(kos_sem_create(KOS_SEM_COUNT_MAX - 1, &sem) == 0);
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

        // The POSITIVE half: the semaphore at index 3 with nothing at 1 or 2. Its post
        // takes it to the ceiling, where main's own post is refused.
        uint16_t const at3[] = {CH_IRQ};
        w = kos::thread::create_caps(capdest_probe, nullptr, "cdp", 15, caps, 1,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, at3);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(kos_sem_post(sem) == -KOS_EOVERFLOW);
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // --- One driver per line: a second claim on a bound line is refused --------
    constexpr int OWNERSHIP_LINE = KICKOS_IRQ_FREE_BASE + 5;
    void t_irq_ownership()
    {
        kos_cap_t owned = KOS_CAP_NONE;
        kos_cap_t stolen = KOS_CAP_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&owned) and hold.cap(&stolen));
        TAP_ASK_LINE(&owned, .irq_line = OWNERSHIP_LINE);
        // main has AUTH_IRQ, so this tests an occupied line rather than missing authority.
        TAP_CHECK(kos_irq_claim(OWNERSHIP_LINE, KOS_IRQ_EDGE, &stolen) == -KOS_EBUSY
                  and stolen == KOS_CAP_NONE);
    }

    // --- A raise on a masked line is latched, and dispatched only at the rearm ---
    // A second source's bit is pending before the wait, so the wait answers under the lock its
    // own rearm runs under: the bits it answers are what was dispatched before that rearm.
    void t_irq_raise_masked()
    {
        kos_cap_t line = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        kos_cap_t other = KOS_CAP_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&line) and hold.bound(&note) and hold.cap(&other));
        TAP_ASK_LINE(&line, .notifies = 1, .irq_line = DISCARD_LINE);
        note = notify_for_line(line);
        TAP_CHECK(note != KOS_CAP_NONE);
        TAP_CHECK(kos_notify_badge(note, 1, &other) == 0);
        TAP_CHECK(kos_notify_bind(note) == 0);
        uint32_t first = 0;
        uint32_t before = 0;
        uint32_t after = 0;
        uint32_t acked = 0;
        TAP_CHECK(kos_irq_ack(line) == 0);
        TAP_CHECK(kos_irq_raise(line) == 0);
        TAP_CHECK(kos_notify_wait(note, NOTE_ALL, STALL_TOLERANT_US, &first) == 0 and first == 1u);
        TAP_CHECK(kos_irq_raise(line) == 0);
        TAP_CHECK(kos_notify(other) == 0);
        TAP_CHECK(kos_notify_wait(note, NOTE_ALL, STALL_TOLERANT_US, &before) == 0
                  and before == 2u);
        TAP_CHECK(kos_notify_wait(note, NOTE_ALL, STALL_TOLERANT_US, &after) == 0 and after == 1u);
        TAP_CHECK(kos_irq_raise(line) == 0);
        TAP_CHECK(kos_irq_ack(line) == 0);
        TAP_CHECK(kos_notify_wait(note, NOTE_ALL, STALL_TOLERANT_US, &acked) == 0
                  and acked == 1u);
        TAP_CHECK(kos_notify_unbind(note) == 0);
    }

    // --- The tier-1 mint is gated on KOS_AUTH_IRQ ------------------------------
    // The refusal MUST be witnessed from a worker, not from main: the suite declares
    // KOS_AUTH_IRQ, so a main-side claim tests the GRANT and can never see the refusal.
    // Keep this to ONE new static: on a 16 KiB part .bss added here shrinks the arena.
    int g_claimgate_rc = 0;
    constexpr int CLAIM_GATE_LINE = KICKOS_IRQ_FREE_BASE + 7;

    void claimgate_worker(void*) // UNPRIVILEGED, authority 0
    {
        kos_cap_t line = KOS_CAP_NONE;
        g_claimgate_rc = kos_irq_claim(CLAIM_GATE_LINE, KOS_IRQ_EDGE, &line);
    }
    void t_irq_claim_gate()
    {
        TAP_ASK(.workers = 1);
        g_claimgate_rc = 0;
        kos_cap_t owned = KOS_CAP_NONE;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.cap(&owned) and hold.thread(&w));
        w = kos::thread::create_caps(claimgate_worker, nullptr, "claimgate", 15, nullptr, 0);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        // EPERM, not EBUSY: the line is free, so only the authority gate can refuse it.
        TAP_CHECK(g_claimgate_rc == -KOS_EPERM);
        // The refusal left NOTHING behind: main can still claim that same line.
        TAP_CHECK(kos_irq_claim(CLAIM_GATE_LINE, KOS_IRQ_EDGE, &owned) == 0);
        TAP_CHECK(hold.close(&owned) == 0);
    }

    // --- A line comes back when its holder dies --------------------------------
    // A dying thread's line cap is dropped and the binding slot freed, so the SAME line is
    // claimable again. Without that release the line returns -KOS_EBUSY forever.
    constexpr int RECLAIM_LINE = KICKOS_IRQ_FREE_BASE + 8;

    // Do NOT rewrite this as wait-then-raise: main's raise would land on a masked line and the arm
    // would deadlock instead of testing. The ack reaches the ARMED state with no event needed.
    void reclaim_worker(void*) // holds the delegated line cap at CH_IRQ, then EXITS
    {
        kos_irq_ack(CH_IRQ);
    }
    // The object the reclaim line signals, held by main for the arm's length: the line must
    // signal SOMEWHERE before the worker's ack can arm it.
    kos_cap_t g_reclaim_note = KOS_CAP_NONE;
    void t_irq_reclaim()
    {
        kos_cap_t first = KOS_CAP_NONE;
        kos_cap_t second = KOS_CAP_NONE;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.cap(&first) and hold.cap(&g_reclaim_note) and hold.cap(&second)
                  and hold.thread(&w));
        TAP_ASK_LINE(&first, .workers = 1, .notifies = 1, .irq_line = RECLAIM_LINE);
        g_reclaim_note = notify_for_line(first);
        TAP_CHECK(g_reclaim_note != KOS_CAP_NONE);
        // done@1, line@3, index 2 deliberately EMPTY.
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {first, KOS_CAP_WAIT}};
        uint16_t const dest[] = {CH_DONE, CH_IRQ};
        w = irq_spawn(reclaim_worker, nullptr, "reclaim", 15, caps, 2,
                      KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0,
                      /*authority=*/0, dest);
        TAP_CHECK(w.valid());
        // main drops its copy BEFORE the worker dies, so the worker's exit is what takes the
        // refcount to zero. Closing after would not prove that DEATH releases the line. The
        // NOTIFICATION's own name goes too: the line's attachment holds a reference of its
        // own, so a copy left open here would keep the object alive past the line.
        TAP_CHECK(hold.close(&first) == 0);
        TAP_CHECK(hold.close(&g_reclaim_note) == 0);
        // The claim is that DEATH releases the line, so the edge has to be the death itself:
        // sched::exit_current runs cap_teardown BEFORE it wakes a joiner, so a join that
        // returns 0 has the worker's line cap already dropped.
        TAP_CHECK(hold.joined());
        // A refusal that never lifts means death did not release the line.
        TAP_CHECK(irq_claim_await(RECLAIM_LINE, &second) == 0);
        TAP_CHECK(hold.close(&second) == 0);
    }

    // Notification handover across a death: the first server binds, takes a raise it never
    // consumes, and dies. The pending bits live in the object, not the TCB, so the
    // successor's bind finds them. main's capability keeps the object and the line alive
    // across the gap.
    constexpr int HANDOVER_LINE = KICKOS_IRQ_FREE_BASE + 8;
    Atomic<int32_t, Order::RELAXED> g_ho_first{-99}; // event inherited from the first server
    Atomic<int32_t, Order::RELAXED> g_ho_second{-99}; // event after rearming
    Atomic<uint32_t, Order::RELAXED> g_ho_bound{0};
    kos_cap_t g_ho_release = KOS_CAP_NONE;

    // caps: done@1 (unposted), ready@2, note@3, release@4. The LINE is not delegated: the
    // successor's own wait is what rearms it, which is the second half of this arm.
    void ho_leaver(void*)
    {
        g_ho_bound = static_cast<uint32_t>(kos_notify_bind(CH_IRQ) == 0);
        kos_sem_post(CH_READY); // bound; main may raise
        kos_sem_wait(CH_REL, KOS_TIMEOUT_NONE);   // exit with the raise unconsumed and the binding still held
    }
    void ho_successor(void*)
    {
        (void)kos_notify_bind(CH_IRQ);
        uint32_t bits = 0;
        // main raises nothing between binds, so this must be the pending raise the leaver
        // left behind.
        g_ho_first = kos_notify_wait(CH_IRQ, NOTE_ALL, STALL_TOLERANT_US, &bits);
        kos_sem_post(CH_READY); // main may raise the second event
        g_ho_second = kos_notify_wait(CH_IRQ, NOTE_ALL, STALL_TOLERANT_US, &bits);
    }
    void t_irq_server_handover()
    {
        g_ho_first = -99;
        g_ho_second = -99;
        g_ho_bound = 0;
        kos_cap_t line = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        kos::thread::Handle first;
        kos::thread::Handle second;
        ArmHold hold;
        TAP_HOLD(hold.cap(&line) and hold.cap(&note) and hold.cap(&g_irq_ready)
                  and hold.cap(&g_ho_release) and hold.thread(&first) and hold.thread(&second));
        TAP_ASK_LINE(&line, .workers = 1, .sems = 2, .notifies = 1, .irq_line = HANDOVER_LINE);
        note = notify_for_line(line);
        TAP_CHECK(note != KOS_CAP_NONE);
        // Arm the claimed line before the raise.
        TAP_CHECK(kos_irq_ack(line) == 0);
        TAP_CHECK(kos_sem_create(0, &g_irq_ready) == 0);
        TAP_CHECK(kos_sem_create(0, &g_ho_release) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_irq_ready, CH_FULL},
                                {note, CH_FULL}, {g_ho_release, CH_FULL}};
        first = irq_spawn(ho_leaver, nullptr, "hoL", 15, caps, 4);
        TAP_CHECK(first.valid());
        TAP_CHECK(kos_sem_wait(g_irq_ready, STALL_TOLERANT_US) == 0); // first server is bound
        TAP_CHECK(kos_irq_raise(line) == 0);
        kos_sem_post(g_ho_release); // exit with the event pending
        TAP_CHECK(thread_end(&first) == 0);
        second = irq_spawn(ho_successor, nullptr, "hoS", 15, caps, 4);
        TAP_CHECK(second.valid());
        // Successor finished its first wait
        TAP_CHECK(kos_sem_wait(g_irq_ready, STALL_TOLERANT_US) == 0);
        TAP_CHECK(kos_irq_raise(line) == 0);
        TAP_CHECK(hold.joined());
        tap::diag("handover: bound %u, inherited %d, next raise %d",
                  static_cast<unsigned>(g_ho_bound.load()),
                  static_cast<int>(g_ho_first.load()),
                  static_cast<int>(g_ho_second.load()));
        TAP_CHECK(g_ho_bound.load() == 1u);
        TAP_CHECK(g_ho_first.load() == 0);
        TAP_CHECK(g_ho_second.load() == 0);
        TAP_CHECK(hold.close(&line) == 0);
        TAP_CHECK(hold.close(&note) == 0);
    }

    // --- First-arm discards a raise latched before it ---------------------------
    // A raise that lands before a driver's first wait is latched on the line the claim left
    // masked, and the first arm must discard that stale latch (arch_irq_clear_pending) or the
    // first irq.wait() phantom-wakes on garbage. This is the one tier-1 arm where main must not
    // pre-arm the line: pre-arming moves the discard into main and stops testing the driver's
    // own first arm.
    int g_stale_seen = 0;
    constexpr int STALE_LINE = KICKOS_IRQ_FREE_BASE + 6;

    void stale_driver(void*)
    {
        auto note = kos::Notification::adopt(CH_NOTE);
        note.bind();
        kos_sem_post(CH_READY); // g_irq_ready
        note.wait(NOTE_ALL);    // first arm discards the latch, then MUST block
        g_stale_seen++;         // only after main raises a REAL event
    }
    void t_irq_stale_register()
    {
        g_stale_seen = 0;
        kos_cap_t irq = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        kos::thread::Handle drv;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_irq_ready) and hold.cap(&irq) and hold.cap(&note)
                  and hold.thread(&drv));
        TAP_ASK_LINE(&irq, .workers = 1, .sems = 1, .notifies = 1, .irq_line = STALE_LINE);
        TAP_CHECK(kos_sem_create(0, &g_irq_ready) == 0);
        // Neither the claim nor the attach below arms the line, so both raises latch for the
        // driver's own first wait to discard.
        int const early = kos_irq_raise(irq);
        int const again = kos_irq_raise(irq);
        note = notify_for_line(irq);
        TAP_CHECK(note != KOS_CAP_NONE);
        kos_cap_grant caps[] = {{g_done, CH_FULL},
                                {g_irq_ready, CH_FULL},
                                {irq, KOS_CAP_WAIT},
                                {note, CH_FULL}};
        drv = irq_spawn(stale_driver, nullptr, "staleirq", IRQ_DRV_PRIO, caps, 4);
        TAP_CHECK(drv.valid());
        TAP_CHECK(hold.close(&note) == 0);
        // Driver is bound, about to take its first wait
        TAP_CHECK(kos_sem_wait(g_irq_ready, STALL_TOLERANT_US) == 0);
        bool no_phantom = true;
#if KICKOS_KERNEL_CORES > 1
        // The no-phantom half is not witnessed above one kernel core. A wake that must not
        // happen raises no event, and the liveness half below cannot separate the two: under
        // the phantom the driver wakes on the stale latch, and main's later raise then lands
        // on a line with no waiter, leaving the same count. Checked on every one-core preset.
        tap::partial("a phantom wake is a wake that must NOT happen");
#else
        TAP_CHECK(others_parked());
        no_phantom = (g_stale_seen == 0);
#endif
        // Liveness, on every core count. main cannot observe the driver REACH its first
        // wait, and that wait is what arms the line: a raise landing before it is cleared by
        // the arm itself, which is the behaviour under test. So raise until one is delivered,
        // bounded.
        uint64_t const deadline = kos_clock_now() + IRQ_EDGE_BUDGET_NS;
        TAP_CHECK(kos_irq_raise(irq) == 0);
        while (g_stale_seen == 0 and kos_clock_now() <= deadline)
        {
            kos_sleep_ns(IRQ_EDGE_POLL_NS);
            TAP_CHECK(kos_irq_raise(irq) == 0);
        }
        bool const live = (g_stale_seen == 1);

        TAP_CHECK(early == 0 and again == 0);
        TAP_CHECK(no_phantom);
        TAP_CHECK(live);
        TAP_CHECK(hold.joined());
    }
#endif

    // --- Caller-owned thread stack: spawn takes a caller-provided stack, and rejects an
    // undersized or misaligned one -------------------------------------------------------
    kos_cap_t g_cstk_ep = KOS_CAP_NONE;
    void caller_stack_worker(void*) // caps: g_cstk_ep(SIGNAL) at CH_DONE
    {
        (void)kos_send(CH_DONE, "", 0);
    }

    // 0 where the spawn's child reported on g_cstk_ep and exited, else the spawn's refusal or
    // the receive's error: a child killed on its first push never reports. A child it saw gone
    // is forgotten: its slot is the next spawn's, and a join through the old handle then fails.
    int cstk_ran(kos::thread::Handle& t)
    {
        if (not t.valid())
        {
            return t.error();
        }
        char b = 0;
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, g_cstk_ep, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
        int32_t const rc = kos_reply_recv(KOS_CAP_NONE, &b, kos_call_lens_pack(0, 0), &o);
        int const jrc = thread_end(&t);
        if (rc < 0)
        {
            return rc;
        }
        return jrc;
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
    // caps: g_done at CH_DONE, the release at CH_READY
    void caller_stack_errno_worker(void*)
    {
        char* end = nullptr;
        (void)strtol("10", &end, 99);
        kos_sem_post(CH_DONE);
        kos_sem_wait(CH_READY, KOS_TIMEOUT_NONE);
        g_cstk_odd_errno = errno;
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
    constexpr uint32_t CSTK_STK = cstk_size();
    // Where regions are powers of two the block is aligned to its own size, and 16 bytes past
    // the stack would double it. Elsewhere the 16 stand: the smallest parts' arena measurements
    // were taken with them.
#if defined(KICKOS_MPU_MIN_REGION_CFG) and defined(KICKOS_MPU_REGION_POW2_CFG) \
    and KICKOS_MPU_MIN_REGION_CFG != 0 and KICKOS_MPU_REGION_POW2_CFG != 0
    constexpr uint32_t CSTK_RESERVE = CSTK_STK;
#else
    constexpr uint32_t CSTK_RESERVE = CSTK_STK + 16u;
#endif

    // The block the caller-stack arms share.
    constexpr RamAsk CSTK_RAM = {CSTK_RESERVE};
    void* cstk_stack(void* raw)
    {
        return reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(raw) + 15u) & ~uintptr_t{15});
    }

#if not KICKOS_HAVE_ASPACE
    void* g_cstk_grant_base = nullptr;
    uint32_t g_cstk_grant_size = 0;
    int g_cstk_grant_rc = 1;
    void caller_stack_grant_worker(void*) // caps: g_cstk_ep(SIGNAL) at CH_DONE
    {
        g_cstk_grant_rc = kos_mem_self_grant(g_cstk_grant_base, g_cstk_grant_size, 0);
        (void)kos_send(CH_DONE, "", 0);
    }

    // A child whose stack shares its block with another region of its own: refused where the
    // MPU faults on an address two regions cover, run everywhere else.
    void t_caller_stack_overlap()
    {
        TAP_ASK(.endpoints = 1);
        void* const raw = st_ram<CSTK_RAM, 0>();
        TAP_CHECK(raw != nullptr);
        void* const stk = cstk_stack(raw);
        uint32_t const size = CSTK_STK;
        uint32_t const reserve = CSTK_RESERVE;
        int want = 0;
        if (ST_MPU_OVERLAP == ARCH_MPU_OVERLAP_FAULTS)
        {
            want = -KOS_EINVAL;
        }
        kos_task_t task = KOS_TASK_NONE;
        kos::thread::Handle td;
        kos::thread::Handle tw;
        kos::thread::Handle tg;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_cstk_ep) and hold.task(&task) and hold.thread(&td)
                  and hold.thread(&tw) and hold.thread(&tg));
        TAP_CHECK(kos_endpoint_create(&g_cstk_ep) == 0);
        kos_cap_grant caps[] = {{g_cstk_ep, KOS_CAP_SIGNAL}};
        // The stack inside its task's data region.
        int const trc = kos_task_create(stk, size, 0, &task);
        int drc = want;
        if (trc == 0)
        {
            td = kos::thread::create(caller_stack_worker, nullptr, "cstkD", 10, KOS_POLICY_FIFO,
                                     0, false, nullptr, 0, stk, size, nullptr, 0, caps, 1, 0,
                                     nullptr, task);
            drc = cstk_ran(td);
            TAP_CHECK(kos_task_kill(task) == 0);
            task = KOS_TASK_NONE;
        }
        else
        {
            tap::partial("data-region half not run (task create rc %d)", trc);
        }
        // The stack under a window of the same spawn.
        int wrc = want;
        if (drc == want)
        {
            kos_window const w = {reinterpret_cast<uintptr_t>(stk), size, KOS_WINDOW_MEMORY, 0};
            tw = kos::thread::create(caller_stack_worker, nullptr, "cstkW", 10, KOS_POLICY_FIFO,
                                     0, false, nullptr, 0, stk, size, &w, 1, caps, 1);
            wrc = cstk_ran(tw);
        }
        // A self-grant over the running thread's own stack and past it: an overlap only where a
        // region is not a power of two, the reserve then being wider than the stack.
        g_cstk_grant_base = raw;
        g_cstk_grant_size = reserve;
        g_cstk_grant_rc = 1;
        int grc = 0;
        if (drc == want and wrc == want)
        {
            tg = kos::thread::create(caller_stack_grant_worker, nullptr, "cstkG", 10,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, stk, size, nullptr, 0,
                                     caps, 1, KOS_AUTH_MEMORY);
            grc = cstk_ran(tg);
        }
        TAP_CHECK(hold.close(&g_cstk_ep) == 0);
        TAP_CHECK(drc == want);
        TAP_CHECK(wrc == want);
        TAP_CHECK(grc == 0);
        if (ST_MPU_OVERLAP == ARCH_MPU_OVERLAP_FAULTS and reserve != size)
        {
            TAP_CHECK(g_cstk_grant_rc == -KOS_EINVAL);
        }
        else
        {
            TAP_CHECK(g_cstk_grant_rc == 0);
        }
    }
#endif

    void t_caller_stack()
    {
        TAP_ASK(.sems = 1, .endpoints = 1);
        // Reject a non-null, tiny + misaligned caller stack: -KOS_EINVAL, not run or corrupt.
        TAP_CHECK(kos::thread::create(caller_stack_worker, nullptr, "badstk", 10, KOS_POLICY_FIFO,
                                      0, false, nullptr, 0, reinterpret_cast<void*>(0x1), 8).error()
                  == -KOS_EINVAL);
        // Accept a properly-sized, aligned caller-owned stack.
        constexpr uint32_t STK = CSTK_STK;
        void* const raw = st_ram<CSTK_RAM, 0>();
        TAP_CHECK(raw != nullptr);
        // Allocation grants nothing, and where a backend translates the block is not even
        // mapped: a child started on it would fault on its first push. A region backend
        // seats the child's stack descriptor itself, and a grant there would hold one of
        // main's own descriptors for the life of the run.
#if KICKOS_HAVE_ASPACE
        TAP_CHECK(kos_mem_self_grant(raw, CSTK_RESERVE, 0) == 0);
#endif
        void* const stk = cstk_stack(raw);
        // One KICKOS_STACK_ALIGN unit below the least floor a spawn charges, with an aligned
        // base, so only the size check can reject it.
        TAP_CHECK(kos::thread::create(caller_stack_worker, nullptr, "undf", 10, KOS_POLICY_FIFO,
                                      0, false, nullptr, 0, stk,
                                      KICKOS_MIN_STACK_SIZE + CSTK_CARVE_LEAST - 16u, nullptr,
                                      0, nullptr, 0).error()
                  == -KOS_EINVAL);
        kos::thread::Handle t;
        kos::thread::Handle to;
        kos_cap_t release = KOS_CAP_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_cstk_ep) and hold.cap(&release) and hold.thread(&t)
                  and hold.thread(&to));
        TAP_CHECK(kos_endpoint_create(&g_cstk_ep) == 0);
        kos_cap_grant caps[] = {{g_cstk_ep, KOS_CAP_SIGNAL}};
        t = kos::thread::create(caller_stack_worker, nullptr, "cstk", 10, KOS_POLICY_FIFO, 0, false,
                                nullptr, 0, stk, STK, nullptr, 0, caps, 1);
        int const ran = cstk_ran(t);
        TAP_CHECK(hold.close(&g_cstk_ep) == 0);
        TAP_CHECK(ran == 0);
#if !KICKOS_MEMORY_ENFORCED
#if KICKOS_LIBC_REENT
        // The worker sets its errno through libc, parks, and reads it back after this thread has
        // set a different one: a thread whose libc state was never seated shares this one's.
        TAP_CHECK(kos_sem_create(0, &release) == 0);
        g_cstk_odd_errno = 0;
        kos_cap_grant ocaps[] = {{g_done, CH_FULL}, {release, CH_FULL}};
        to = kos::thread::create(caller_stack_errno_worker, nullptr, "cstkO", 10, KOS_POLICY_FIFO,
                                 0, false, nullptr, 0, g_cstk_odd, CSTK_ODD, nullptr, 0, ocaps, 2);
        TAP_CHECK(to.valid());
        TAP_CHECK(kos_sem_wait(g_done, STALL_TOLERANT_US) == 0);
        char* end = nullptr;
        (void)strtol("99999999999999999999999999", &end, 10);
        int const mine = errno;
        kos_sem_post(release);
        TAP_CHECK(hold.joined());
        TAP_CHECK(mine == ERANGE);
        TAP_CHECK(g_cstk_odd_errno == EINVAL);
        TAP_CHECK(errno == ERANGE);
#else
        TAP_CHECK(kos_endpoint_create(&g_cstk_ep) == 0);
        kos_cap_grant ocaps[] = {{g_cstk_ep, KOS_CAP_SIGNAL}};
        to = kos::thread::create(caller_stack_worker, nullptr, "cstkO", 10, KOS_POLICY_FIFO, 0,
                                 false, nullptr, 0, g_cstk_odd, CSTK_ODD, nullptr, 0, ocaps, 1);
        int const oran = cstk_ran(to);
        TAP_CHECK(hold.close(&g_cstk_ep) == 0);
        TAP_CHECK(oran == 0);
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
    int g_dreadback = -1;
    constexpr int DOM_SENTINEL = 0x5A5A;
    constexpr RamAsk DOM_RAM = {256};
    void dom_writer(void*) // caps: g_dwrote@1 (CH_DONE)
    {
        (void)kos_mem_self_grant(const_cast<int*>(g_dshared), 256, 0);
        *g_dshared = DOM_SENTINEL;
        kos_sem_post(CH_DONE);     // g_dwrote
    }
    void dom_reader(void*) // caps: g_dwrote@1 (CH_DONE)
    {
        (void)kos_mem_self_grant(const_cast<int*>(g_dshared), 256, 0);
        kos_sem_wait(CH_DONE, KOS_TIMEOUT_NONE);    // g_dwrote: after the writer stored the sentinel
        g_dreadback = *g_dshared;
    }
    void t_domain_share()
    {
        TAP_ASK(.workers = 2, .sems = 1);
        g_dshared = static_cast<volatile int*>(st_ram<DOM_RAM, 0>());
        TAP_CHECK(g_dshared != nullptr);
        kos::thread::Handle w;
        kos::thread::Handle r;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_dwrote) and hold.thread(&w) and hold.thread(&r));
        // main's own reach.
        TAP_CHECK(kos_mem_self_grant(const_cast<int*>(g_dshared), 256, 0) == 0);
        *g_dshared = 0;
        g_dreadback = -1;
        TAP_CHECK(kos_sem_create(0, &g_dwrote) == 0);
        kos_cap_grant wcaps[] = {{g_dwrote, CH_FULL}};
        kos_cap_grant rcaps[] = {{g_dwrote, CH_FULL}};
        w = kos::thread::create_caps(dom_writer, nullptr, "domW", 10, wcaps, 1, KOS_POLICY_FIFO,
                                     0, false, nullptr, 0, KOS_AUTH_MEMORY);
        r = kos::thread::create_caps(dom_reader, nullptr, "domR", 10, rcaps, 1, KOS_POLICY_FIFO,
                                     0, false, nullptr, 0, KOS_AUTH_MEMORY);
        TAP_CHECK(w.valid() and r.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(g_dreadback == DOM_SENTINEL);
    }

#if KICKOS_MEMORY_ENFORCED
    // One task cannot use another's reservation through self-grant, spawn data, or a
    // caller-supplied stack: -KOS_EPERM on MMU and MPU backends alike. The worker's zero-size
    // self-grant, refused for its size and not its authority, excludes missing authority as
    // the cause.
    enum
    {
        XG_AUTH = 0,     // a zero-size self-grant: -KOS_EINVAL, the authority admitted
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
    constexpr RamAsk XG_RAM = {XG_STK};
    void xg_noop(void*) {}
    void xg_worker(void* arg) // caps: done@1, E(SIGNAL)@2
    {
        int32_t rep[XG_WORDS] = {1, 1, 1, 1};
        rep[XG_AUTH] = kos_mem_self_grant(arg, 0, 0);
        // main's address, carried as a NUMBER and never dereferenced: this task does not
        // reach it, and the point is that it cannot make it reach it.
        rep[XG_FOREIGN] = kos_mem_self_grant(arg, XG_BLK, 0);
        rep[XG_MEMBASE] = kos::thread::create(xg_noop, nullptr, "xgmem", 10, KOS_POLICY_FIFO,
                                              0, false, arg, XG_STK).error();
        rep[XG_STACK] = kos::thread::create(xg_noop, nullptr, "xgstk", 10, KOS_POLICY_FIFO,
                                            0, false, nullptr, 0, arg, XG_STK).error();
        (void)kos_send(2, rep, sizeof(rep));
    }
    void t_cross_task_block()
    {
        TAP_ASK(.workers = 1, .tasks = 1, .endpoints = 1);
        kos_cap_t ep = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.task(&t) and hold.thread(&w));
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        void* const theirs = st_ram<XG_RAM, 0>();
        TAP_CHECK(theirs != nullptr);
        // main maps it, so the worker names a range that really is live somewhere.
        TAP_CHECK(kos_mem_self_grant(theirs, XG_STK, 0) == 0);
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &t) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {ep, KOS_CAP_SIGNAL}};
        w = kos::thread::create_caps(xg_worker, theirs, "xgrnt", 10, caps, 2, KOS_POLICY_FIFO, 0,
                                     false, nullptr, 0, KOS_AUTH_MEMORY, nullptr, t);
        TAP_CHECK(w.valid());
        int32_t rep[XG_WORDS] = {1, 1, 1, 1};
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, ep, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
        TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, rep, kos_call_lens_pack(0, sizeof(rep)), &o)
                  == static_cast<int32_t>(sizeof(rep)));
        TAP_CHECK(hold.joined());
        TAP_CHECK(rep[XG_AUTH] == -KOS_EINVAL);
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
        kos_sem_wait(1, KOS_TIMEOUT_NONE);
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
                kos_cap_t ep, WroSeen* seen, kos::thread::Handle* c)
    {
        g_wro_put = put;
        g_wro_wrote = 0;
        *seen = {-1, -1};
        kos_window const w = {reinterpret_cast<uintptr_t>(blk), size, KOS_WINDOW_MEMORY, flags};
        kos_cap_grant const caps[] = {{ep, KOS_CAP_SIGNAL}};
        *c = kos::thread::create(wro_child, blk, "wro", 10, KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                 nullptr, 0, &w, 1, caps, 1, 0, nullptr, task);
        int rc = c->error();
        if (rc == 0)
        {
            struct kos_reply_recv_opts o;
            kos_reply_recv_opts_init(&o, ep, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
            (void)kos_reply_recv(KOS_CAP_NONE, seen, kos_call_lens_pack(0, sizeof(*seen)), &o);
            rc = thread_end(c);
        }
        return rc;
    }
    constexpr RamAsk WRO_RAM = {ST_GRANULES(1), ST_GRANULES(1), ST_GRANULES(1), ST_GRANULES(1)};
    void t_window_memory_ro()
    {
        size_t const g = arena_granule();
        TAP_ASK(.workers = 1, .tasks = 1, .sems = 1, .endpoints = 1);
        void* const blk = st_ram<WRO_RAM, 0>();
        TAP_CHECK(blk != nullptr);
        kos_task_t t = KOS_TASK_NONE;
        kos_task_t nc_task = KOS_TASK_NONE;
        kos_task_t cached_task = KOS_TASK_NONE;
        kos_cap_t ep = KOS_CAP_NONE;
        kos_cap_t gate = KOS_CAP_NONE;
        kos::thread::Handle child;
        kos::thread::Handle holder;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.cap(&gate) and hold.task(&t) and hold.task(&nc_task)
                 and hold.task(&cached_task) and hold.thread(&child) and hold.thread(&holder));
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &t) == 0);
        uint32_t const size = static_cast<uint32_t>(g);
        WroSeen seen = {-1, -1};
        TAP_CHECK(wro_run(blk, size, 0, WRO_BYTE, KOS_TASK_NONE, ep, &seen, &child) == 0);
        TAP_CHECK(g_wro_wrote == 1);
        TAP_CHECK(seen.moved == KICKOS_HAVE_ASPACE);
        TAP_CHECK(wro_run(blk, size, KOS_WINDOW_RO, 0, t, ep, &seen, &child) == 0);
        TAP_CHECK(seen.read == WRO_BYTE);
        TAP_CHECK(task_end(&t) == 0);
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
        child = kos::thread::create(wro_child, blk, "wroa", 10, KOS_POLICY_FIFO, 0, false, blk,
                                    size, nullptr, 0, nullptr, 0, dcaps, 1);
        TAP_CHECK(child.valid());
        seen = {-2, -2};
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, ep, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
        (void)kos_reply_recv(KOS_CAP_NONE, &seen, kos_call_lens_pack(0, sizeof(seen)), &o);
        TAP_CHECK(thread_end(&child) == 0);
        TAP_CHECK(seen.read == -1 and seen.moved == -1);
#if not KICKOS_HAVE_ASPACE
        kos_window const ro = {reinterpret_cast<uintptr_t>(blk), size, KOS_WINDOW_MEMORY,
                               KOS_WINDOW_RO};
        g_wro_sys = -1;
        child = kos::thread::create(wro_sys_child, blk, "wros", 10, KOS_POLICY_FIFO, 0, false,
                                    blk, size, nullptr, 0, &ro, 1);
        // Only an MPU letting the higher-numbered window decide reads the block as the kernel
        // does; elsewhere the pair would fault the child or let it write.
        if (ST_MPU_OVERLAP == -1 or ST_MPU_OVERLAP == ARCH_MPU_OVERLAP_HIGHER)
        {
            TAP_CHECK(child.valid() and thread_end(&child) == 0);
            TAP_CHECK(g_wro_sys == -KOS_EFAULT);
        }
        else
        {
            TAP_CHECK(child.error() == -KOS_EINVAL);
        }
#endif
        TAP_CHECK(wro_run(blk, size, 0, -1, KOS_TASK_NONE, ep, &seen, &child) == 0);
        TAP_CHECK(seen.read == WRO_BYTE);
        TAP_CHECK(hold.close(&ep) == 0);
        // A block held uncached keeps that type: a cacheable window over it, and the owner's
        // own cacheable grant, are both refused while the uncached window lives.
        void* const other = st_ram<WRO_RAM, 1>();
        TAP_CHECK(kos_sem_create(0, &gate) == 0);
        kos_window const nc = {reinterpret_cast<uintptr_t>(other), size, KOS_WINDOW_MEMORY,
                               KOS_WINDOW_UNCACHED};
        kos_cap_grant const hcaps[] = {{gate, KOS_CAP_WAIT}};
        holder = kos::thread::create(wro_hold_child, nullptr, "wroh", 10, KOS_POLICY_FIFO, 0,
                                     false, nullptr, 0, nullptr, 0, &nc, 1, hcaps, 1);
        if (holder.error() == -KOS_ENOTSUP)
        {
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
        kos_sem_post(gate);
        TAP_CHECK(thread_end(&holder) == 0);
        TAP_CHECK(hold.close(&gate) == 0);
        // One spawn asking for a block as cacheable task data and through an uncached window
        // is refused too, though neither mapping exists when its windows are admitted.
        void* const both = st_ram<WRO_RAM, 2>();
        kos_window const nc_both = {reinterpret_cast<uintptr_t>(both), size, KOS_WINDOW_MEMORY,
                                    KOS_WINDOW_UNCACHED};
        TAP_CHECK(kos::thread::create(wro_hold_child, nullptr, "wrob", 10, KOS_POLICY_FIFO, 0,
                                      false, both, size, nullptr, 0, &nc_both, 1).error()
                  == -KOS_EBUSY);
        // A task holds its data with no member yet: a second empty task taking the same block
        // with another memory type is refused as a spawn would be.
        void* const shared = st_ram<WRO_RAM, 3>();
        TAP_CHECK(kos_task_create(shared, size, KOS_MEM_NOCACHE, &nc_task) == 0);
        TAP_CHECK(kos_task_create(shared, size, 0, &cached_task) == -KOS_EBUSY);
    }
#endif

    // --- MMIO grant boundary: privileged-only + encodable-only -------------------
    // The positive grant is HW-only, so this arm pins the two refusals: a window one MPU
    // descriptor cannot cover exactly, and any grant attempted by an UNPRIVILEGED caller.
    // The sim's arch_mpu_region_encodable admits ONE window (its fake register block) and
    // neither of these names it, so both halves still refuse there.
    int g_mmio_unpriv_rc = -2;
    void mmio_noop(void*) {}
    void mmio_unpriv_worker(void*)
    {
        // Unprivileged caller: the privilege gate must refuse the MMIO grant.
        g_mmio_unpriv_rc = kos::thread::create(mmio_noop, nullptr, "mmiochild", 10,
                                               KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                               nullptr, 0,
                                               DeviceWindow(0x1000u, 4096).list(), 1)
                               .error();
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
        g_mmio_unpriv_rc = -2;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.thread(&w));
        w = kos::thread::create(mmio_unpriv_worker, nullptr, "mmioW", 10);
        if (not w.valid())
        {
            // The cases above already ran, so this stays `ok` and names the dropped half.
            tap::partial("unprivileged half not run (thread pool too small)");
            return;
        }
        TAP_CHECK(hold.joined());
        TAP_CHECK(g_mmio_unpriv_rc == -KOS_EPERM);
    }

    // --- stack_base arena containment (unprivileged self-grant) -----------------
    // The stack_base grant is the ONE unprivileged path that reaches an MPU region. Without
    // an arena bound an unprivileged thread spawns a child with stack_base in peripheral
    // space or kernel SRAM: an R|W window the MMIO gate would refuse. Enforcing backends
    // only: the escalation needs a region descriptor to land in.
#if KICKOS_HAVE_MPU
    int g_stkarena_rc = -2;
    void stkarena_noop(void*) {}
    void stkarena_unpriv_worker(void*)
    {
        // Unprivileged caller; stack_base far above any SRAM arena and naturally aligned
        // (clears the size/align/natural checks) so ONLY the arena bound can reject it.
        g_stkarena_rc = kos::thread::create(stkarena_noop, nullptr, "stkbad", 10,
                                            KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                            reinterpret_cast<void*>(0xE0000000u), 2048)
                            .error();
    }
    void t_stackbase_arena()
    {
        TAP_ASK(.workers = 1);
        g_stkarena_rc = -2;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.thread(&w));
        w = kos::thread::create(stkarena_unpriv_worker, nullptr, "stkW", 10);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(g_stkarena_rc == -KOS_EPERM);
    }

    constexpr int CH_DEVHOLD = 2; // the holder gate, delegated SECOND (done@1, hold@2)
#if defined(KICKOS_ENABLE_SELFTEST)
    // --- Rule 7 at the spawn: a grant outside the arena or over a reserved block -----
    // The predicates themselves are held on the host (grantnocache); these are the two
    // refusals a spawn reaches them by.
    void grant_noop(void*) {}
    void t_grant_reserved()
    {
        // An unprivileged child whose mem_base lies outside the arena is refused with
        // -KOS_EPERM (policy refusal), not -KOS_ENOMEM, before any slot is claimed.
        // 0xE0000000 is 2048-aligned, so ONLY arena containment can reject it.
        auto const mrc = kos::thread::create(grant_noop, nullptr, "membad", 10, KOS_POLICY_FIFO,
                                             0, /*privileged=*/false,
                                             reinterpret_cast<void*>(0xE0000000u), 2048);
        TAP_CHECK(mrc.error() == -KOS_EPERM);
#if defined(KICKOS_SELFTEST_RESERVED_BASE)
        // A device window inside the chip's first reserved block, naturally aligned and a power
        // of two from 32 bytes so every region backend can encode it: only the overlap with the
        // block can refuse it, though main holds the authority a device window needs.
        uintptr_t const rbase = KICKOS_SELFTEST_RESERVED_BASE;
        uint32_t rsize = 32;
        while (2u * rsize <= KICKOS_SELFTEST_RESERVED_SIZE and rbase % (2u * rsize) == 0)
        {
            rsize = 2u * rsize;
        }
        auto const rc = kos::thread::create(grant_noop, nullptr, "rsvd", 10, KOS_POLICY_FIFO, 0,
                                            false, nullptr, 0, nullptr, 0,
                                            DeviceWindow(rbase, rsize).list(), 1);
        TAP_CHECK(rbase % rsize == 0 and rsize <= KICKOS_SELFTEST_RESERVED_SIZE);
        TAP_CHECK(rc.error() == -KOS_EPERM);
#else
        tap::partial("reserved-block spawn not run (board reserves nothing)");
#endif
    }

    // --- One holder per device window (-KOS_EBUSY) --------------------------------
    // A DEV window overlapping one a LIVE domain already holds is refused, matched on
    // RANGES: exact duplicate and partial overlap refuse, adjacent-but-disjoint admits. The
    // holder MUST stay alive for the whole matrix and parks on a semaphore, a reschedule
    // between two spawns otherwise letting it exit and turning every refusal into an
    // admission. The window is DISCOVERED; the sim admits exactly one, so a WIN-sized search
    // there reports PARTIAL while on any other enforcing board it must FAIL.
    void devexcl_hold(void*) // caps: done@1, hold@2
    {
        kos_sem_wait(CH_DEVHOLD, KOS_TIMEOUT_NONE); // hold the window until main releases it
    }
    void t_dev_window_exclusive()
    {
        constexpr uint32_t WIN = 0x100u; // pow2 >= 32: encodable on PMSAv7/v8 and byte-granular SYSMPU
        // Step 2*WIN so both `base` and its sibling `base + WIN` stay WIN-aligned: PMSA needs
        // natural alignment, so an unaligned base would be refused as unencodable, not held.
        TAP_ASK(.workers = 2, .sems = 1);
        kos_cap_t gate = KOS_CAP_NONE;
        kos::thread::Handle holder;
        kos::thread::Handle adj;
        // Every spawn the arm expects refused: one admitted by mistake parks holding its window.
        kos::thread::Handle stray;
        ArmHold hold;
        TAP_HOLD(hold.cap(&gate) and hold.thread(&holder) and hold.thread(&adj)
                 and hold.thread(&stray));
        TAP_CHECK(kos_sem_create(0, &gate) == 0);
        kos_cap_grant hcaps[] = {{g_done, CH_FULL}, {gate, CH_FULL}};
        uintptr_t win = 0;
        bool any_admissible = false;
        int last = 0;
        for (uintptr_t b = 0x40000000u; b < 0x40100000u; b += 2u * WIN)
        {
            // Both windows at once, by a child that exits at once: admitted only where each is
            // admissible and nothing holds either.
            kos_window const pair[] = {DeviceWindow(b, WIN).w, DeviceWindow(b + WIN, WIN).w};
            stray = kos::thread::create(grant_noop, nullptr, "devfree", 10, KOS_POLICY_FIFO, 0,
                                        false, nullptr, 0, nullptr, 0, pair, 2);
            last = stray.error();
            if (last == -KOS_EBUSY or last == -KOS_ENOMEM)
            {
                any_admissible = true;
            }
            if (last == -KOS_ENOMEM)
            {
                break;
            }
            if (not stray.valid())
            {
                continue; // held, reserved, alias or not encodable on this chip
            }
            any_admissible = true;
            TAP_CHECK(thread_end(&stray) == 0);
            // An exited thread holds no window, so a refusal here is a window never given back.
            holder = kos::thread::create(devexcl_hold, nullptr, "devheld", 10, KOS_POLICY_FIFO,
                                         0, /*privileged=*/false, nullptr, 0, nullptr, 0,
                                         DeviceWindow(b, WIN).list(), 1, hcaps, 2);
            if (not holder.valid())
            {
                tap::fail("the window a joined child held was refused rc %d", holder.error());
                return;
            }
            win = b;
            break;
        }
        if (not any_admissible)
        {
            // Positively the sim, so a new arch with no DEV encoder fails loudly here.
#if KICKOS_ARCH_SIM
            // The sim admits exactly one DEV window shape (64 KiB), never a WIN-sized one.
            // Assert that premise instead of skipping: the sim gate reads a skip as an arm
            // that stopped running (FAIL_REGULAR_EXPRESSION "# skipped: [1-9]").
            stray = kos::thread::create(devexcl_hold, nullptr, "devnone", 10, KOS_POLICY_FIFO, 0,
                                        false, nullptr, 0, nullptr, 0,
                                        DeviceWindow(0x40000000u, WIN).list(), 1, hcaps, 2);
            TAP_CHECK(not stray.valid());
            tap::partial("board mints no DEV window; exclusivity runs on enforcing boards "
                         "(e.g. the qemu base variant)");
#else
            // An enforcing board with no admissible DEV base means the discovery loop or the
            // spawn's admission regressed. Passing here would silently drop every -KOS_EBUSY
            // assertion below while the gate stayed green.
            tap::fail("no DEV-admissible window pair in [0x40000000, 0x40100000) (last rc %d)",
                      last);
#endif
            return;
        }
        if (win == 0)
        {
            if (last != -KOS_EBUSY)
            {
                tap::fail("DEV pair spawn refused rc %d, expected 0 or -KOS_EBUSY", last);
                return;
            }
            tap::skip("every DEV-admissible window is already held (pair rc %d)", last);
            return;
        }
        // 0. One list naming the free neighbour twice: nothing else holds it, so only the
        //    check within the list can refuse it.
        kos_window const twice[] = {DeviceWindow(win + WIN, WIN).w,
                                    DeviceWindow(win + WIN, WIN).w};
        stray = kos::thread::create(devexcl_hold, nullptr, "devtwice", 10, KOS_POLICY_FIFO, 0,
                                    false, nullptr, 0, nullptr, 0, twice, 2, hcaps, 2);
        TAP_CHECK(stray.error() == -KOS_EBUSY);
        // 1. Exact duplicate of the live holder's window: refused, and specifically
        //    EBUSY, not EPERM (the window is admissible) and not ENOMEM (the pool has
        //    room; the refusal lands before a slot is claimed).
        stray = kos::thread::create(devexcl_hold, nullptr, "devdup", 10, KOS_POLICY_FIFO, 0,
                                    false, nullptr, 0, nullptr, 0, DeviceWindow(win, WIN).list(),
                                    1, hcaps, 2);
        TAP_CHECK(stray.error() == -KOS_EBUSY);
        // 2. Partial overlap: the upper half of the held window. Its own base/size are
        //    independently admissible, so only the overlap scan can refuse it.
        stray = kos::thread::create(devexcl_hold, nullptr, "devpart", 10, KOS_POLICY_FIFO, 0,
                                    false, nullptr, 0, nullptr, 0,
                                    DeviceWindow(win + WIN / 2u, WIN / 2u).list(), 1, hcaps, 2);
        TAP_CHECK(stray.error() == -KOS_EBUSY);
        // 3. Adjacent but disjoint (base == held last + 1): admitted. This is the mk64f PIT
        //    CH2 shape: a grant flush against a block, which must not be read as overlapping.
        adj = kos::thread::create(devexcl_hold, nullptr, "devadj", 10, KOS_POLICY_FIFO, 0, false,
                                  nullptr, 0, nullptr, 0, DeviceWindow(win + WIN, WIN).list(), 1,
                                  hcaps, 2);
        TAP_CHECK(adj.valid());
        // The post is what frees the windows, and the joins say they are free.
        kos_sem_post(gate);
        kos_sem_post(gate);
        TAP_CHECK(thread_end(&holder) == 0 and thread_end(&adj) == 0);
        // The SAME grant refused above must now succeed, so the refusal tracked live holders
        // and not the address, and it succeeds as one list with its neighbour, whose second
        // entry is then held as the first is.
        kos_window const both[] = {DeviceWindow(win, WIN).w, DeviceWindow(win + WIN, WIN).w};
        holder = kos::thread::create(devexcl_hold, nullptr, "devagain", 10, KOS_POLICY_FIFO, 0,
                                     false, nullptr, 0, nullptr, 0, both, 2, hcaps, 2);
        TAP_CHECK(holder.valid());
        stray = kos::thread::create(devexcl_hold, nullptr, "devsecond", 10, KOS_POLICY_FIFO, 0,
                                    false, nullptr, 0, nullptr, 0,
                                    DeviceWindow(win + WIN, WIN).list(), 1, hcaps, 2);
        TAP_CHECK(stray.error() == -KOS_EBUSY);
        kos_sem_post(gate);
        TAP_CHECK(hold.joined());
    }
#endif // KICKOS_ENABLE_SELFTEST
#endif // KICKOS_HAVE_MPU

#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
#if not KICKOS_ARCH_SIM
    extern "C" char _sdata[]; // the first word of the kernel's .data, from the link
#endif
    // Memory no unprivileged thread reaches: [0] a word of the kernel's own data, [1] an arena
    // block main reserved and nobody was granted. [0] stays null on the sim, whose pointer checks
    // admit its whole host image, kernel data included.
    constexpr int UNREACHED = 2;
    void* g_unreached[UNREACHED] = {};
    constexpr RamAsk UNREACHED_RAM = {64};
#define CD_RAM UNREACHED_RAM
    bool unreached_ready()
    {
#if not KICKOS_ARCH_SIM
        g_unreached[0] = _sdata;
#endif
        g_unreached[1] = st_ram<UNREACHED_RAM, 0>();
        return g_unreached[1] != nullptr;
    }
    // The half the sim cannot show, declared rather than passed.
    void unreached_partial()
    {
        if (g_unreached[0] == nullptr)
        {
            tap::partial("no kernel word is refused where the pointer checks admit the host image");
        }
    }
#else
#define CD_RAM ST_NO_RAM
#endif

    // --- Confused-deputy readable-buffer floor ---------------------------------
    // A user pointer syscall_dispatch READS must lie in memory the unprivileged caller could
    // itself reach. A rodata string literal MUST be accepted and a pointer into no granted
    // region MUST be rejected, never read; both run from a spawned unprivileged worker. The
    // positive half is non-vacuous only when PAIRED with the unreached-block negative below.
    char const CD_LIT[] = "# [confdep] unpriv rodata buffer accepted by the readable floor\n";
    // Plain: main reads these only after joining the worker.
    // worker: kconsole_write(rodata literal) -> a count or -KOS_EBUSY
    long g_cd_lit_rc = -99;
    int g_cd_goodspawn = -99;  // worker: spawn rc of a child NAMED from .rodata
    int g_cd_goodname_ran = 0; // that child ran (name-copy path did not break spawn)
    kos_cap_t g_cd_kidsem = KOS_CAP_NONE; // grandchild -> worker handoff
#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
    // One per g_unreached subject.
    int g_cd_neg_ran[UNREACHED] = {};        // the negative half ran over that subject
    long g_cd_bad_rc[UNREACHED] = {};        // worker: kconsole_write(subject) -> -KOS_EFAULT
    int g_cd_badname_spawn[UNREACHED] = {};  // spawn rc with the subject as NAME -> expect 0
    int g_cd_badname_ran[UNREACHED] = {};    // that child ran (kernel walked the bad name safely)
#endif
    void cd_kid(void* arg) // caps: g_cd_kidsem@1 (CH_DONE), delegated by cd_worker
    {
        *static_cast<int*>(arg) = 1;
        kos_sem_post(CH_DONE); // g_cd_kidsem (this grandchild's delegated cap)
    }
    void cd_worker(void*) // UNPRIVILEGED
    {
        size_t const lit_len = strlen(CD_LIT);
        int32_t const lit_took
            = KICKOS_KCONSOLE_MEASURED(ANSWER, CD_LIT, lit_len);
        g_cd_lit_rc = lit_took;
        write_rest(CD_LIT, lit_len, lit_took);

        // cd_worker creates its OWN sem (unprivileged create is allowed) and RE-delegates
        // it to a grandchild: nested delegation requires the source cap carry TRANSFER,
        // which sem_create grants. g_cd_kidsem is cd_worker's cap value (its table).
        kos_sem_create(0, &g_cd_kidsem);
        kos_cap_grant kidcaps[] = {{g_cd_kidsem, CH_FULL}};  // grandchild's index 1
        // A child NAMED from .rodata: the kernel bounds + copies the string. Userspace
        // cannot read a TCB name back, so acceptance shows as the child running.
        // Each grandchild is joined before the next spawn: it holds a slot and a stack the next spawn
        // may need.
        auto const good = kos::thread::create_caps(cd_kid, &g_cd_goodname_ran, "cdgood", 9,
                                                   kidcaps, 1);
        g_cd_goodspawn = good.error();
        if (g_cd_goodspawn == 0)
        {
            kos_sem_wait(g_cd_kidsem, KOS_TIMEOUT_NONE);
            (void)good.join();
        }
#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
        for (int i = 0; i < UNREACHED; i++)
        {
            void* const bad = g_unreached[i];
            if (bad == nullptr)
            {
                continue;
            }
            // Bogus console buffer: rejected, and never read (a wrong-accept would return 8,
            // having read memory the caller cannot reach).
            g_cd_bad_rc[i] = KICKOS_KCONSOLE_MEASURED(ANSWER, bad, 8);
            // Bogus NAME pointer: the kernel must bound the walk (no fault), drop the
            // name, and still spawn the child.
            auto const badname = kos::thread::create_caps(cd_kid, &g_cd_badname_ran[i],
                                                          static_cast<char const*>(bad), 9,
                                                          kidcaps, 1);
            g_cd_badname_spawn[i] = badname.error();
            if (g_cd_badname_spawn[i] == 0)
            {
                kos_sem_wait(g_cd_kidsem, KOS_TIMEOUT_NONE);
                (void)badname.join();
            }
            g_cd_neg_ran[i] = 1;
        }
#endif
        kos_sem_destroy(g_cd_kidsem); // close cd_worker's own cap
    }

#if defined(KICKOS_ENABLE_SELFTEST)
#if KICKOS_KERNEL_LINE_COUNT > 0 or defined(__aarch64__)
    // Claims `line` and closes what it took. 0, the claim's refusal, or -KOS_EINVAL where a
    // refusal handed back a capability, a banked line took a raise, or the close was refused.
    int irq_claim_and_close(int line, bool raise_banked)
    {
        kos_cap_t cap = KOS_CAP_NONE;
        int const rc = irq_claim_await(line, &cap);
        if (rc != 0)
        {
            if (cap != KOS_CAP_NONE)
            {
                return -KOS_EINVAL;
            }
            return rc;
        }
        int verdict = 0;
        if (raise_banked and kos_irq_raise(cap) != -KOS_ENOTSUP)
        {
            verdict = -KOS_EINVAL;
        }
        if (kos_handle_close(cap) != 0)
        {
            verdict = -KOS_EINVAL;
        }
        return verdict;
    }
#endif

    // --- A line the arch dispatches to a kernel vector of its own --------------
    // The ordinary line below is what says the refusal is that line's and not every line's.
    void t_irq_kernel_line_reserved()
    {
        kos_cap_t line = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        ArmHold hold;
        TAP_HOLD(hold.cap(&line) and hold.bound(&note));
        TAP_ASK_LINE(&line, .notifies = 1, .irq_line = CLAIM_GATE_LINE);
        int named = 0;
        [[maybe_unused]] bool timer_named = false;
#if KICKOS_KERNEL_LINE_COUNT > 0
        constexpr int KERNEL_LINES[] = {KICKOS_KERNEL_LINES};
        for (int const kernel_line : KERNEL_LINES)
        {
            tap::diag("arch-owned irq line: %d", kernel_line);
            TAP_CHECK(irq_claim_and_close(kernel_line, false) == -KOS_EPERM);
            named++;
#if defined(__aarch64__)
            if (kernel_line == kickos::arm64::PPI_EL1_PHYS_TIMER)
            {
                timer_named = true;
            }
#endif
        }
#endif
#if defined(__aarch64__)
        // The timer's PPI, where the chip file states the timer with no line.
        if (not timer_named)
        {
            TAP_CHECK(irq_claim_and_close(kickos::arm64::PPI_EL1_PHYS_TIMER, false)
                      == -KOS_EPERM);
            named++;
        }
        // A banked line pends on the raising core alone, so every SGI is claimable but never
        // raised, except the doorbell's, which the kernel rings and nobody claims.
        constexpr int GIC_SGIS = 16;
        constexpr int GIC_SGI_DOORBELL = 0; // arch_arm64_gicv2.cc and arch_arm64_gicv3.cc
        uint32_t refused = 0;
        int other = 0;
        for (int sgi = 0; sgi < GIC_SGIS; sgi++)
        {
            int const rc = irq_claim_and_close(sgi, true);
            if (rc == -KOS_EPERM)
            {
                tap::diag("arch-owned irq line: %d", sgi);
                refused |= 1u << sgi;
            }
            else if (rc != 0)
            {
                tap::diag("SGI %d: claim, raise or close answered %d", sgi, rc);
                other++;
            }
        }
        uint32_t expected = 0;
        if (KICKOS_NUM_CORES > 1 or KICKOS_AMP_NODE)
        {
            expected = 1u << GIC_SGI_DOORBELL;
        }
        TAP_CHECK(refused == expected and other == 0);
#endif
        if (named == 0)
        {
            // A partial and never a plain pass: the control below still runs, so this is not a
            // skip, but a chip whose kernel takes no line of its own leaves the refusal no
            // subject.
            tap::partial("this chip routes every line through the first-level ISR");
        }
        // CLAIM_GATE_LINE, free outside its own arm: attached, armed, fired and closed here.
        note = notify_for_line(line);
        TAP_CHECK(note != KOS_CAP_NONE);
        TAP_CHECK(kos_notify_bind(note) == 0);
        TAP_CHECK(kos_irq_ack(line) == 0); // a claim leaves the line masked
        // The control for the refusal above: an ordinary line is claimed, raised and delivered.
        TAP_CHECK(kos_irq_raise(line) == 0);
        uint32_t bits = 0;
        TAP_CHECK(kos_notify_wait(note, NOTE_ALL, STALL_TOLERANT_US, &bits) == 0 and bits != 0);
        TAP_CHECK(kos_irq_ack(line) == 0);
        TAP_CHECK(kos_notify_unbind(note) == 0);
        TAP_CHECK(hold.close(&note) == 0);
        TAP_CHECK(hold.close(&line) == 0);
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
        TAP_ASK(.workers = 1, .tasks = 1);
        kos_task_t t = KOS_TASK_NONE;
        kos::thread::Handle above;
        kos::thread::Handle at;
        ArmHold hold;
        TAP_HOLD(hold.task(&t) and hold.thread(&above) and hold.thread(&at));
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &t) == 0);
        TAP_CHECK(kos_task_sched_grant(t, PC_CEILING, 0) == 0);
        above = kos::thread::create_caps(pc_idle_worker, nullptr, "pcabv", PC_CEILING + 1,
                                         nullptr, 0, KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                         nullptr, t);
        TAP_CHECK(not above.valid());
        TAP_CHECK(above.error() == -KOS_EPERM);
        at = kos::thread::create_caps(pc_idle_worker, nullptr, "pcat", PC_CEILING, nullptr, 0,
                                      KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr, t);
        TAP_CHECK(at.valid());
        TAP_CHECK(hold.joined());
    }

    enum
    {
        PN_MADE = 0,  // the member got a task of its own to narrow
        PN_ABOVE = 1, // a ceiling above the member's own
        PN_SAME = 2,  // ... and one equal to it
        PN_WORDS = 3
    };
    // Refusals only: the placement reads do not reach the kernel at one kernel core and this arm
    // runs on every posture.
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
        TAP_ASK(.workers = 1, .tasks = 2, .endpoints = 1);
        kos_task_t t = KOS_TASK_NONE;
        kos::thread::Handle m;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_pl_ep) and hold.task(&t) and hold.thread(&m));
        TAP_CHECK(kos_endpoint_create(&g_pl_ep) == 0);
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &t) == 0);
        TAP_CHECK(kos_task_sched_grant(t, PC_CEILING, 0) == 0);
        // The same task, still empty: narrowing-only is checked against the TASK's ceiling
        // too, so main widening one it already narrowed is refused although main's own
        // ceiling admits it.
        TAP_CHECK(kos_task_sched_grant(t, PC_CEILING + 1, 0) == -KOS_EPERM);
        kos_cap_grant caps[] = {{g_pl_ep, KOS_CAP_SIGNAL}};
        // The member creates a task of its own to narrow, so it holds the task authority.
        m = kos::thread::create_caps(pc_narrow_worker, nullptr, "pcnar", PC_CEILING, caps, 1,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, KOS_AUTH_TASKS,
                                     nullptr, t);
        TAP_CHECK(m.valid());
        int32_t rep[PN_WORDS] = {0, 0, 0};
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, g_pl_ep, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
        TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, rep, kos_call_lens_pack(0, sizeof(rep)), &o)
                  == static_cast<int32_t>(sizeof(rep)));
        TAP_CHECK(hold.joined());
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
    constexpr size_t SB_LEN = 32;
    char g_sb_buf[SB_LEN];
    int32_t g_sb_sent = 1; // 1 is no answer a send can give
    void sb_sender(void*)  // caps: done@1, E(SIGNAL)@2
    {
        g_sb_sent = kos_send(2, g_sb_buf, SB_LEN);
    }
    void t_ipc_one_buffer_both_ends()
    {
        TAP_ASK(.workers = 1, .endpoints = 1);
        kos_cap_t ep = KOS_CAP_NONE;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.thread(&w));
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        for (size_t i = 0; i < SB_LEN; i++)
        {
            g_sb_buf[i] = static_cast<char>('a' + (i & 15u));
        }
        g_sb_sent = 1;
        // No task named, so the child is a thread of main's task and the array below is one
        // address in one space.
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {ep, KOS_CAP_SIGNAL}};
        w = kos::thread::create_caps(sb_sender, nullptr, "sbuf", 10, caps, 2);
        TAP_CHECK(w.valid());
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, ep, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
        int32_t const got =
            kos_reply_recv(KOS_CAP_NONE, g_sb_buf, kos_call_lens_pack(0, SB_LEN), &o);
        TAP_CHECK(hold.joined());
        tap::diag("one buffer both ends: recv %d, send %d",
                  static_cast<int>(got), static_cast<int>(g_sb_sent));
        TAP_CHECK(got == -KOS_EFAULT);
        TAP_CHECK(g_sb_sent == -KOS_EFAULT);
    }

    void t_confused_deputy()
    {
#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
        TAP_CHECK(unreached_ready());
#endif
        TAP_ASK(.workers = 2, .sems = 1);
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.thread(&w));
#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
        for (int i = 0; i < UNREACHED; i++)
        {
            g_cd_neg_ran[i] = 0;
            g_cd_bad_rc[i] = -99;
            g_cd_badname_spawn[i] = -99;
            g_cd_badname_ran[i] = 0;
        }
#endif
        w = kos::thread::create(cd_worker, nullptr, "cdwork", 10);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        // Positive (every backend): the floor accepted an unprivileged caller's rodata
        // pointer. kos_kconsole_write waits for the console, and answers -KOS_EBUSY when a
        // published driver serves the caller and -KOS_EFAULT when it rejects the buffer.
        if (g_cd_lit_rc < 0 and g_cd_lit_rc != -KOS_EBUSY)
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
        for (int i = 0; i < UNREACHED; i++)
        {
            if (g_unreached[i] != nullptr)
            {
                TAP_CHECK(g_cd_neg_ran[i] == 1);
                TAP_CHECK(g_cd_bad_rc[i] == -KOS_EFAULT); // bogus buffer rejected, never read
                TAP_CHECK(g_cd_badname_spawn[i] == 0 and g_cd_badname_ran[i] == 1);
            }
        }
        unreached_partial();
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
    Atomic<kos_cap_t, Order::RELAXED> g_ep_rreply{0x55};
    Atomic<int, Order::RELAXED> g_ep_rcap{64}; // capacity the recv worker passes
    Atomic<int32_t, Order::RELAXED> g_ep_sn{-99}; // worker send return

    void ep_recv_worker(void*) // caps: done@1, E@2 (unpriv)
    {
        // Keep the recv buffer thread-private: a global one is also accepted
        // (user_writable_ok has a static-data fallback, covered by endpoint_zero_accept) and
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
        g_ep_rreply = o.info.reply_cap;
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
    }
    void ep_send_worker(void*) // caps: done@1, E@2 (unpriv)
    {
        g_ep_sn = kos_send(2, EP_MSG, strlen(EP_MSG));
    }

    void t_endpoint_rendezvous()
    {
        TAP_ASK(.workers = 1, .endpoints = 1);
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.thread(&w));
        size_t const mlen = strlen(EP_MSG);
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, CH_FULL}};

        // (A) receiver parks first; sender (main) delivers into the parked buffer.
        g_ep_rn = -99; g_ep_rbadge = 0xdeadu; g_ep_rreply = 0x55; g_ep_rcap = 64;
        w = irq_spawn(ep_recv_worker, nullptr, "eprx", TAP_PRIO_PARKS, caps, 2);
        TAP_CHECK(w.valid());
        kos_yield();
        int32_t sc = kos_send_timed(g_ep, EP_MSG, mlen, STALL_TOLERANT_US);
        TAP_CHECK(sc == static_cast<int32_t>(mlen));
        TAP_CHECK(thread_end(&w) == 0);
        int32_t const ep_rn_a = g_ep_rn;
        TAP_CHECK(ep_rn_a == static_cast<int32_t>(mlen) and memcmp(g_ep_rbuf, EP_MSG, mlen) == 0);
        uint32_t const ep_rbadge = g_ep_rbadge;
        TAP_CHECK(ep_rbadge == 0); // badge always written on success; unbadged is 0
        kos_cap_t const ep_rreply = g_ep_rreply;
        TAP_CHECK(ep_rreply == KOS_CAP_NONE); // a plain send carries no reply cap

        // (B) sender parks first; receiver (main) takes from the parked buffer.
        g_ep_sn = -99;
        w = irq_spawn(ep_send_worker, nullptr, "eptx", TAP_PRIO_PARKS, caps, 2);
        TAP_CHECK(w.valid());
        kos_yield();
        char rbuf[64];
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, g_ep, 0, STALL_TOLERANT_US);
        o.info.badge = 0xdeadu; // sentinel for write-back
        o.info.reply_cap = 0x55;
        int32_t rc = kos_reply_recv(KOS_CAP_NONE, rbuf, kos_call_lens_pack(0, sizeof(rbuf)), &o);
        TAP_CHECK(rc == static_cast<int32_t>(mlen) and memcmp(rbuf, EP_MSG, mlen) == 0);
        TAP_CHECK(o.info.badge == 0);
        TAP_CHECK(o.info.reply_cap == KOS_CAP_NONE); // a plain send carries no reply cap
        TAP_CHECK(thread_end(&w) == 0);
        int32_t const ep_sn = g_ep_sn;
        TAP_CHECK(ep_sn == static_cast<int32_t>(mlen));

        // (C) zero-length is a valid signal, not an error.
        g_ep_rn = -99; g_ep_rcap = 64;
        w = irq_spawn(ep_recv_worker, nullptr, "epz", TAP_PRIO_PARKS, caps, 2);
        TAP_CHECK(w.valid());
        kos_yield();
        TAP_CHECK(kos_send_timed(g_ep, EP_MSG, 0, STALL_TOLERANT_US) == 0);
        TAP_CHECK(thread_end(&w) == 0);
        int32_t const ep_rn_c = g_ep_rn;
        TAP_CHECK(ep_rn_c == 0);

        // (D) truncation: a capacity below the message length.
        g_ep_rn = -99; g_ep_rcap = 4;
        w = irq_spawn(ep_recv_worker, nullptr, "eptr", TAP_PRIO_PARKS, caps, 2);
        TAP_CHECK(w.valid());
        kos_yield();
        TAP_CHECK(kos_send_timed(g_ep, EP_MSG, mlen, STALL_TOLERANT_US) == 4);
        TAP_CHECK(thread_end(&w) == 0);
        int32_t const ep_rn_d = g_ep_rn;
        TAP_CHECK(ep_rn_d == 4 and memcmp(g_ep_rbuf, EP_MSG, 4) == 0);

        TAP_CHECK(hold.close(&g_ep) == 0);
    }

    // --- Oversize reject + bad cap (main only; no parking) -----------------------
    void t_endpoint_reject()
    {
        TAP_ASK(.endpoints = 1);
        char big[KOS_EP_MSG_MAX + 8];
        memset(big, 'x', sizeof(big));
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep));
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
        TAP_CHECK(hold.close(&g_ep) == 0);
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
    }
    void t_endpoint_rights()
    {
        TAP_ASK(.workers = 1, .endpoints = 1);
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.thread(&w));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        g_ep_wait_send_rc = -99; g_ep_signal_recv_rc = -99;
        // Two narrowed caps to the same endpoint: WAIT-only at index 2, SIGNAL-only at 3.
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}, {g_ep, EP_SIGNAL_ONLY}};
        w = kos::thread::create_caps(ep_rights_worker, nullptr, "eprt", 12, caps, 3,
                                     KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        int const ep_wait_send_rc = g_ep_wait_send_rc;
        int const ep_signal_recv_rc = g_ep_signal_recv_rc;
        TAP_CHECK(ep_wait_send_rc == -KOS_EACCES);
        TAP_CHECK(ep_signal_recv_rc == -KOS_EACCES);
        TAP_CHECK(hold.close(&g_ep) == 0);
    }

    // --- The handout right: an endpoint with no receiver answers by whether one may come --
    // Main narrows its creator cap to SIGNAL, TRANSFER and HANDOUT. No receiver remains, so a
    // send answers -KOS_EAGAIN at once instead of parking, and main cannot receive through it.
    // It still seats a receiver, granting WAIT it does not hold. Until that worker first
    // waits on the endpoint a send answers -KOS_EAGAIN as well; once it waits, the worker
    // takes the next send. With the worker gone a send answers -KOS_EAGAIN again; with the
    // handout right dropped, -KOS_ECONNREFUSED, and a WAIT grant from the cap is refused.
    constexpr uint8_t EP_HANDOUT_ONLY = KOS_CAP_SIGNAL | KOS_CAP_TRANSFER | KOS_CAP_HANDOUT;
    // Bounds the send to a seated receiver that has not waited, which must not park at all.
    constexpr uint32_t HO_SEATED_TIMEOUT_US = 50000u;
    Atomic<int32_t, Order::RELAXED> g_ho_rn{-99};
    kos_cap_t g_ho_gate = KOS_CAP_NONE;
    void ho_receiver(void*) // caps: done@1, E(WAIT)@2, gate@3
    {
        kos_sem_wait(3, KOS_TIMEOUT_NONE);
        char buf[sizeof(EP_MSG)];
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, 2, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        g_ho_rn = kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &o);
    }
    int32_t ho_seat(kos::thread::Handle* out)
    {
        kos_cap_grant const caps[] = {{g_done, CH_FULL}, {g_ep, KOS_CAP_WAIT}, {g_ho_gate, CH_FULL}};
        *out = irq_spawn(ho_receiver, nullptr, "hoR", TAP_PRIO_PARKS, caps, 3);
        return out->error();
    }
    void t_endpoint_handout()
    {
        TAP_ASK(.workers = 1, .sems = 1, .endpoints = 1);
        kos::thread::Handle w;
        kos::thread::Handle b;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.cap(&g_ho_gate) and hold.thread(&w)
                 and hold.thread(&b));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        TAP_CHECK(kos_sem_create(0, &g_ho_gate) == 0);
        int32_t const mlen = static_cast<int32_t>(strlen(EP_MSG));
        TAP_CHECK(kos_cap_narrow(g_ep, EP_HANDOUT_ONLY) == 0);
        TAP_CHECK(kos_send_timed(g_ep, EP_MSG, strlen(EP_MSG), STALL_TOLERANT_US)
                  == -KOS_EAGAIN);
        char buf[sizeof(EP_MSG)];
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, g_ep, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
        TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &o)
                  == -KOS_EACCES);
        g_ho_rn = -99;
        TAP_CHECK(ho_seat(&w) == 0);
        kos_yield();
        TAP_CHECK(kos_send_timed(g_ep, EP_MSG, strlen(EP_MSG), HO_SEATED_TIMEOUT_US)
                  == -KOS_EAGAIN);
        kos_sem_post(g_ho_gate);
        TAP_CHECK(kos_send_timed(g_ep, EP_MSG, strlen(EP_MSG), STALL_TOLERANT_US) == mlen);
        TAP_CHECK(thread_end(&w) == 0);
        TAP_CHECK(g_ho_rn.load() == mlen);
        TAP_CHECK(kos_send_timed(g_ep, EP_MSG, strlen(EP_MSG), STALL_TOLERANT_US)
                  == -KOS_EAGAIN);
        TAP_CHECK(kos_cap_narrow(g_ep, KOS_CAP_SIGNAL | KOS_CAP_TRANSFER) == 0);
        TAP_CHECK(kos_send_timed(g_ep, EP_MSG, strlen(EP_MSG), STALL_TOLERANT_US)
                  == -KOS_ECONNREFUSED);
        TAP_CHECK(ho_seat(&b) == -KOS_EACCES);
        TAP_CHECK(hold.close(&g_ep) == 0);
    }

    // --- A parked sender woken by the last receiver leaving ----------------------------------
    // Main's creator cap is the only receiver; a worker parks sending. Main narrows WAIT away
    // while keeping the handout right: the narrow drops the last receiver as a close would, and
    // the parked sender is answered what a new caller would be, -KOS_EAGAIN. Then main closes
    // the cap, the handout right going with it, and the parked sender is refused.
    Atomic<int32_t, Order::RELAXED> g_hp_rc{-99};
    void hp_sender(void*) // caps: done@1, E(SIGNAL)@2
    {
        g_hp_rc = kos_send(2, EP_MSG, strlen(EP_MSG)); // parks: a receiver exists, none waits
    }
    void t_endpoint_handout_parked()
    {
        TAP_ASK(.workers = 1, .endpoints = 1);
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.thread(&w));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        g_hp_rc = -99;
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        w = irq_spawn(hp_sender, nullptr, "hpS", TAP_PRIO_PARKS, caps, 2);
        TAP_CHECK(w.valid());
        kos_yield();
        TAP_CHECK(kos_cap_narrow(g_ep, EP_HANDOUT_ONLY) == 0);
        TAP_CHECK(hold.joined());
        int32_t const hp_rc = g_hp_rc;
        TAP_CHECK(hp_rc == -KOS_EAGAIN);
        TAP_CHECK(hold.close(&g_ep) == 0);

        g_hp_rc = -99;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        caps[1].source_cap = g_ep;
        w = irq_spawn(hp_sender, nullptr, "hpS2", TAP_PRIO_PARKS, caps, 2);
        TAP_CHECK(w.valid());
        kos_yield();
        TAP_CHECK(hold.close(&g_ep) == 0);
        TAP_CHECK(hold.joined());
        int32_t const refused_rc = g_hp_rc;
        TAP_CHECK(refused_rc == -KOS_ECONNREFUSED);
    }

    // --- A receiver taking nothing, against a writer offering the rest again ---------------
    // A rendezvous consumes the receive it completes, a zero-byte one included, so every offer
    // after the first waits for the receiver to receive again.
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
        int const cores = kos_task_cores(KOS_TASK_NONE);
        if (cores <= 0)
        {
            return 1u;
        }
        uint32_t const set = static_cast<uint32_t>(cores);
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
    }

    void t_endpoint_zero_accept()
    {
        TAP_ASK(.workers = 2, .endpoints = 1);
        kos::thread::Handle r;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.thread(&r) and hold.thread(&w));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
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
        r = kos::thread::create_caps(za_receiver, nullptr, "zaR", ZA_RECEIVER_PRIO, rcaps, 2,
                                     KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0, 0,
                                     nullptr, KOS_TASK_NONE, nullptr, 0, core);
        TAP_CHECK(r.valid());
        w = kos::thread::create_caps(za_writer, nullptr, "zaW", ZA_WRITER_START_PRIO, wcaps, 2,
                                     KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0, 0,
                                     nullptr, KOS_TASK_NONE, nullptr, 0, core);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(hold.close(&g_ep) == 0);
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
        TAP_CHECK(raised == 0);
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
    }
    void t_endpoint_send_timeout()
    {
        TAP_ASK(.workers = 1, .endpoints = 1);
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.thread(&w));
        g_ep_timed_returned = 0;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        w = kos::thread::create_caps(ep_timed_worker, nullptr, "eptm", 12, caps, 2,
                                     KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        TAP_CHECK(ep_timed_awaited());
        EpTimedSend r;
        memset(&r, 0, sizeof(r));
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, g_ep, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
        int32_t const n =
            kos_reply_recv(KOS_CAP_NONE, &r, kos_call_lens_pack(0, sizeof(r)), &o);
        TAP_CHECK(n == static_cast<int32_t>(sizeof(r))); // the untimed report parked and landed
        TAP_CHECK(r.rc == -KOS_ETIMEDOUT);            // expired, and no bytes crossed
        // Both clock reads bracket the syscall, so this cannot pass on a deadline the
        // kernel fired immediately.
        TAP_CHECK(r.waited_us >= EP_SEND_TIMEOUT_US);
        TAP_CHECK(hold.joined());
        TAP_CHECK(hold.close(&g_ep) == 0);
    }

    // The three timed-call arms share one deadline, and each waits for its caller's call to
    // return before it acts. Main keeps its WAIT cap for the whole of each, so recv_holders
    // stays 1 and no EPIPE can be mistaken for an expiry.
    constexpr uint32_t EP_CALL_TIMEOUT_US = 4000;
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
    }
    void t_call_timeout_pending()
    {
        TAP_ASK(.workers = 1, .endpoints = 1);
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.thread(&w));
        g_ep_timed_returned = 0;
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        w = kos::thread::create_caps(ep_call_pending_worker, nullptr, "cltp", 12, caps, 2,
                                     KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        char warm[1] = {0};
        struct kos_reply_recv_opts warm_o;
        kos_reply_recv_opts_init(&warm_o, g_ep, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
        TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, warm, kos_call_lens_pack(0, sizeof(warm)),
                                 &warm_o)
                  == 1);
        TAP_CHECK(ep_timed_awaited());
        EpTimedSend r;
        memset(&r, 0, sizeof(r));
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, g_ep, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
        int32_t const n =
            kos_reply_recv(KOS_CAP_NONE, &r, kos_call_lens_pack(0, sizeof(r)), &o);
        TAP_CHECK(n == static_cast<int32_t>(sizeof(r))); // the untimed report parked and landed
        TAP_CHECK(r.rc == -KOS_ETIMEDOUT);            // expired on send_waiters, never taken
        // Both clock reads bracket the syscall, so this cannot pass on a deadline the
        // kernel fired immediately.
        TAP_CHECK(r.waited_us >= EP_CALL_TIMEOUT_US);
        TAP_CHECK(hold.joined());
        TAP_CHECK(hold.close(&g_ep) == 0);
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
        kos_sem_post(3);
        g_cltr_calling = 1;
        uint64_t const t0 = kos_clock_now();
        r.rc = kos_call_timed(2, buf, 4, sizeof(buf), EP_CALL_REPLY_TIMEOUT_US);
        r.waited_us = static_cast<uint32_t>((kos_clock_now() - t0) / 1000ull);
        g_ep_timed_returned = 1;
        (void)kos_send(2, &r, sizeof(r));
    }
    void t_call_timeout_reply()
    {
        TAP_ASK(.workers = 1, .sems = 1, .endpoints = 1);
        g_ep_timed_returned = 0;
        g_cltr_calling = 0;
        kos_cap_t go = KOS_CAP_NONE;
        kos_cap_t reply_cap = KOS_CAP_NONE;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.thread(&w) and hold.cap(&g_ep) and hold.cap(&go) and hold.cap(&reply_cap));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        TAP_CHECK(kos_sem_create(0, &go) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}, {go, KOS_CAP_SIGNAL}};
        w = kos::thread::create_caps(ep_call_reply_worker, nullptr, "cltr", TAP_PRIO_PARKS, caps,
                                     3, KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0, 0,
                                     nullptr, KOS_TASK_NONE, nullptr, 0, TAP_PIN_CORE);
        TAP_CHECK(w.valid());
        TAP_CHECK(kos_sem_wait(go, STALL_TOLERANT_US) == 0);
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
            return;
        }
        TAP_CHECK(got == 4);
        TAP_CHECK(reply_cap != KOS_CAP_NONE); // we hold the reply cap, and never use it
        TAP_CHECK(opts.timeout_us == EP_RECV_GENEROUS_US);
        TAP_CHECK(ep_timed_awaited());
        EpTimedSend r;
        memset(&r, 0, sizeof(r));
        struct kos_reply_recv_opts ro;
        kos_reply_recv_opts_init(&ro, g_ep, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
        int32_t const n =
            kos_reply_recv(KOS_CAP_NONE, &r, kos_call_lens_pack(0, sizeof(r)), &ro);
        TAP_CHECK(n == static_cast<int32_t>(sizeof(r)));
        TAP_CHECK(r.rc == -KOS_ETIMEDOUT); // the deadline crossed the handoff and fired
        TAP_CHECK(r.waited_us >= EP_CALL_REPLY_TIMEOUT_US);
        // The cap outlives the caller by design, so closing it is still the server's job and
        // must succeed with nobody left to wake.
        TAP_CHECK(hold.close(&reply_cap) == 0);
        TAP_CHECK(hold.joined());
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
    }
    void t_reply_stale_caller()
    {
        TAP_ASK(.workers = 1, .endpoints = 1);
        g_ep_timed_returned = 0;
        g_rpst_main_receiving = 0;
        g_rpst_caller_saw = -1;
        kos_cap_t reply_cap = KOS_CAP_NONE;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.thread(&w) and hold.cap(&g_ep) and hold.cap(&reply_cap));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        w = kos::thread::create_caps(ep_reply_stale_worker, nullptr, "rpst", TAP_PRIO_AFTER, caps,
                                     2, KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0, 0,
                                     nullptr, KOS_TASK_NONE, nullptr, 0, TAP_PIN_CORE);
        TAP_CHECK(w.valid());
        char req[8];
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, g_ep, 0, STALL_TOLERANT_US);
        g_rpst_main_receiving = 1;
        int32_t const got =
            kos_reply_recv(KOS_CAP_NONE, req, kos_call_lens_pack(0, sizeof(req)), &opts);
        reply_cap = opts.info.reply_cap;
        TAP_CHECK(g_rpst_caller_saw.load() == 1);
        TAP_CHECK(got == 4 and reply_cap != KOS_CAP_NONE);
        TAP_CHECK(ep_timed_awaited());
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
        kos_reply_recv_opts_init(&opts2, g_ep, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
        int32_t const n = kos_reply_recv(KOS_CAP_NONE, &r, kos_call_lens_pack(0, sizeof(r)), &opts2);
        TAP_CHECK(n == static_cast<int32_t>(sizeof(r)));
        TAP_CHECK(r.rc == -KOS_ETIMEDOUT); // expired on the reply park, not on send_waiters
        TAP_CHECK(r.waited_us >= EP_CALL_TIMEOUT_US);
        TAP_CHECK(hold.joined());
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
    // Main's mark before its first receive, and what the caller read of it before calling.
    Atomic<int, Order::RELAXED> g_sr_main_receiving{0};
    Atomic<int, Order::RELAXED> g_sr_caller_saw{-1};
    void sr_caller(void*) // caps: done@1, lock@2, E1(SIGNAL)@3, go@4
    {
        char buf[8] = {0};
        g_sr_caller_saw = g_sr_main_receiving.load();
        if (kos_call_timed(3, buf, 4, sizeof(buf), EP_CALL_TIMEOUT_US) == -KOS_ETIMEDOUT)
        {
            log_put('T');
        }
        kos_sem_post(4); // the second server may receive: the first call has left the endpoint
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
    }
    void sr_second_server(void*) // caps: done@1, E1(FULL)@2, E2(FULL)@3, go@4
    {
        char buf[8];
        kos_sem_wait(4, KOS_TIMEOUT_NONE);
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
        struct kos_reply_recv_opts opts2;
        kos_reply_recv_opts_init(&opts2, 3, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts2);
        kos_reply(last.reply_cap, SR_GOOD, 4);
    }
    void t_reply_abandoned_cap()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        TAP_ASK(.workers = 2, .sems = 1, .endpoints = 2);
        log_reset();
        g_sr_main_receiving = 0;
        g_sr_caller_saw = -1;
        kos_cap_t ep2 = KOS_CAP_NONE;
        kos_cap_t go = KOS_CAP_NONE;
        kos_cap_t abandoned = KOS_CAP_NONE;
        kos::thread::Handle cl;
        kos::thread::Handle s2;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.cap(&ep2) and hold.cap(&go) and hold.cap(&abandoned)
                 and hold.thread(&cl) and hold.thread(&s2));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        TAP_CHECK(kos_endpoint_create(&ep2) == 0);
        TAP_CHECK(kos_sem_create(0, &go) == 0);
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY},
                                 {go, CH_FULL}};
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_ep, CH_FULL}, {ep2, CH_FULL},
                                 {go, CH_FULL}};
        cl = irq_spawn(sr_caller, nullptr, "srC", TAP_PRIO_AFTER, ccaps, 4);
        s2 = kos::thread::create_caps(sr_second_server, nullptr, "srS", 10, scaps, 4);
        TAP_CHECK(cl.valid() and s2.valid());
        char req[8];
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, g_ep, 0, STALL_TOLERANT_US);
        g_sr_main_receiving = 1;
        int32_t const got = kos_reply_recv(KOS_CAP_NONE, req, kos_call_lens_pack(0, sizeof(req)), &opts); // first call
        abandoned = opts.info.reply_cap;
        TAP_CHECK(g_sr_caller_saw.load() == 1);
        TAP_CHECK(got == 4 and abandoned != KOS_CAP_NONE);
        // Never replied, and never touched again until the token arrives: the deadline
        // expires under us and this cap is abandoned for the rest of the arm.
        char tok[8];
        struct kos_reply_recv_opts opts2;
        kos_reply_recv_opts_init(&opts2, ep2, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
        int32_t const rdy = kos_reply_recv(KOS_CAP_NONE, tok, kos_call_lens_pack(0, sizeof(tok)), &opts2);
        TAP_CHECK(rdy == 3 and memcmp(tok, "rdy", 3) == 0); // the aliasing call is parked
        // Every resolve test but the reply-waiter unlink now passes on this cap.
        TAP_CHECK(kos_reply(abandoned, SR_BAD, 4) == -KOS_ESRCH);
        // Consumed exactly once even on the refusal, so the handle no longer resolves.
        TAP_CHECK(kos_reply(abandoned, SR_BAD, 4) == -KOS_EBADF);
        TAP_CHECK(hold.close(&abandoned) == -KOS_EBADF);
        TAP_CHECK(kos_send_timed(ep2, "go", 2, STALL_TOLERANT_US) == 2); // release the second server's reply
        TAP_CHECK(hold.joined());
        TAP_CHECK(hold.close(&ep2) == 0);
        TAP_CHECK(hold.close(&g_ep) == 0);
        TAP_CHECK(log_eq("TK")); // timed out, then every call answered by its own server
    }

    // --- Timed recv: the deadline expires with nobody sending ---------------------------
    // Runs in main with NO worker: nothing arrives, so the deadline alone is what expires
    // and no cross-domain copy is involved. Both lengths are past KOS_EP_MSG_MAX: a length
    // refused rather than clamped turns the expiry into EINVAL.
    void t_recv_timeout()
    {
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        // KOS_EP_MSG_MAX bytes, so buffer validation does not hide the clamp.
        char buf[KOS_EP_MSG_MAX];
        memset(buf, 0, sizeof(buf));
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, g_ep, 0, EP_CALL_TIMEOUT_US);
        opts.notify = 0u; // no accepted IRQs; output must stay zero
        uint64_t const t0 = kos_clock_now();
        uintptr_t const over = kos_call_lens_pack(2 * KOS_EP_MSG_MAX, 2 * KOS_EP_MSG_MAX);
        int32_t const n = kos_reply_recv(KOS_CAP_NONE, buf, over, &opts);
        uint32_t const waited_us = static_cast<uint32_t>((kos_clock_now() - t0) / 1000ull);
        TAP_CHECK(n == -KOS_ETIMEDOUT); // expired, and no bytes arrived
        TAP_CHECK(waited_us >= EP_CALL_TIMEOUT_US);
        TAP_CHECK(opts.timeout_us == EP_CALL_TIMEOUT_US); // an input, never written back
        // With no accepted IRQs, notify must remain zero.
        TAP_CHECK(opts.notify == 0u);
        TAP_CHECK(hold.close(&g_ep) == 0);
    }

    // --- Malformed arguments to the timed forms -----------------------------------------
    // Every refusal here is decided before anything parks or copies. The EFAULT half needs a
    // pointer the caller does not own, which a privileged
    // caller can never present: it lives in t_endpoint_bound.
    void t_timed_arg_refusals()
    {
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep));
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
        TAP_CHECK(hold.close(&g_ep) == 0);
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
    // caller always runs and parks first. The third is read from the log: stamps tie on a
    // coarse clock.
    Window g_ctr_spin1;
    Atomic<uint32_t, Order::RELAXED> g_ctr_call_at{0};
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
        uint64_t const spin2_from = kos_clock_now();
        while (g_ctr_spoiler_ran.load() == 0u and kos_clock_now() - spin2_from < CTR_GIVE_UP_NS)
        {
        }
        log_put('z');
    }
    void ctr_caller(void*) // caps: done@1, lock@2, E(SIGNAL)@3
    {
        char buf[8] = {0};
        kos_sleep_ns(g_call_unit * 2); // wake mid-spin: the server is not in recv, so we park
        uint32_t const deadline_us = static_cast<uint32_t>((g_call_unit * 8ull) / 1000ull);
        g_ctr_call_at = stamp_now();
        int32_t const rc = kos_call_timed(3, buf, 4, sizeof(buf), deadline_us);
        char c = 'X';
        if (rc == -KOS_ETIMEDOUT)
        {
            c = 'c';
        }
        log_put(c);
    }
    void ctr_spoiler(void*) // caps: done@1, lock@2 (medium prio)
    {
        g_ctr_spoiler_due = stamp_due(g_call_unit * 4);
        kos_sleep_ns(g_call_unit * 4); // ready while the server is boosted, so it must wait
        log_put('m');
        g_ctr_spoiler_ran = 1;
    }
    void t_call_timeout_revert()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        TAP_ASK(.workers = 3, .endpoints = 1);
        log_reset();
        g_call_unit = mtx_time_unit();
        window_reset(g_ctr_spin1);
        g_ctr_call_at = 0;
        g_ctr_spoiler_due = 0;
        g_ctr_spoiler_ran = 0;
        kos::thread::Handle sv;
        kos::thread::Handle cl;
        kos::thread::Handle sp;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.thread(&sv) and hold.thread(&cl) and hold.thread(&sp));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        sv = kos::thread::create_caps(ctr_server, nullptr, "ctrS", 8, scaps, 3);
        cl = kos::thread::create_caps(ctr_caller, nullptr, "ctrC", 20, ccaps, 3);
        sp = kos::thread::create_caps(ctr_spoiler, nullptr, "ctrM", 12, mcaps, 2);
        TAP_CHECK(sv.valid() and cl.valid() and sp.valid());
        char warm[4] = {0};
        TAP_CHECK(kos_send_timed(g_ep, warm, 4, STALL_TOLERANT_US) == 4);
        TAP_CHECK(hold.joined());
        TAP_CHECK(count('a') == 1 and count('u') == 1);
        uint32_t const ctr_call_at = g_ctr_call_at;
        uint32_t const ctr_spoiler_due = g_ctr_spoiler_due;
        if (not window_held(g_ctr_spin1, ctr_call_at))
        {
            skip_window_lost("the timed call landing inside the server's first spin, which is "
                             "what parks it on send_waiters and seats the D2 boost",
                             g_ctr_spin1, ctr_call_at);
            return;
        }
        if (nth('c', 1) < nth('u', 1))
        {
            TAP_SKIP_VACUOUS("the call expired inside the server's first spin, before the "
                             "second, which is the only place the revert is observable");
            return;
        }
        if (not window_reached(g_ctr_spin1, ctr_spoiler_due))
        {
            skip_window_lost("the spoiler falling due before the server's boosted first spin "
                             "ends, which is what the boost has to hold off",
                             g_ctr_spin1, ctr_spoiler_due);
            return;
        }
        TAP_CHECK(hold.close(&g_ep) == 0);
        TAP_CHECK(count('c') == 1 and count('X') == 0); // the call expired, it did not bounce
        TAP_CHECK(count('m') == 1 and count('z') == 1);
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
    }
    void ci_caller(void*) // caps: done@1, lock@2, E(SIGNAL)@3, stage@4, bounced@5
    {
        char buf[8] = {0};
        kos_sem_wait(4, KOS_TIMEOUT_NONE);                       // the server is seated
        kos_sem_post(4);
        g_ci_rc = kos_call(3, buf, 4, sizeof(buf));
        log_put('c');
        // The reject and the park are one masked window, so the bounce returning here is
        // the proof recv#2 is already on recv_waiters.
        kos_sem_post(5);
    }
    void ci_spoiler(void*) // caps: done@1, lock@2, stage@3 (medium prio)
    {
        // Index 3, not the 4 its posters use: holding no endpoint cap shifts the same
        // semaphore one slot down. The wrong index returns at once and this logs first,
        // which reads as a lost boost.
        kos_sem_wait(3, KOS_TIMEOUT_NONE);
        log_put('m');
    }
    void ci_filler(void*) // caps: done@1, lock@2, E(SIGNAL)@3, bounced@4
    {
        char b[4] = {0};
        kos_send(3, b, 4);                     // the only sender recv#1 can see
        kos_sem_wait(4, KOS_TIMEOUT_NONE);
        kos_send(3, b, 4);                     // wakes the parked recv#2
    }
    void t_call_infoless_revert()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        TAP_ASK(.workers = 4, .sems = 2, .endpoints = 1);
        log_reset();
        g_call_unit = mtx_time_unit();
        g_ci_rc = -99;
        kos::thread::Handle sv;
        kos::thread::Handle cl;
        kos::thread::Handle sp;
        kos::thread::Handle fl;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.cap(&g_gate) and hold.cap(&g_ci_bounced)
                 and hold.thread(&sv) and hold.thread(&cl) and hold.thread(&sp)
                 and hold.thread(&fl));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        TAP_CHECK(kos_sem_create(0, &g_gate) == 0);
        // Its own semaphore, not a third post on g_gate: a shared gate hands the caller's pre-call post
        // to the filler.
        TAP_CHECK(kos_sem_create(0, &g_ci_bounced) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY},
                                 {g_gate, CH_FULL}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY},
                                 {g_gate, CH_FULL}, {g_ci_bounced, CH_FULL}};
        kos_cap_grant fcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY},
                                 {g_ci_bounced, CH_FULL}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_gate, CH_FULL}};
        sv = kos::thread::create_caps(ci_server, nullptr, "ciS", 8, scaps, 4);
        cl = kos::thread::create_caps(ci_caller, nullptr, "ciC", 20, ccaps, 5);
        sp = kos::thread::create_caps(ci_spoiler, nullptr, "ciM", 12, mcaps, 3);
        // Above the spoiler: its second send is what wakes the parked server, and that wake
        // must land while the spoiler is still only ready. Below the spoiler, 'm' is logged
        // before the server is woken at all and the revert check below passes either way.
        fl = kos::thread::create_caps(ci_filler, nullptr, "ciF", 14, fcaps, 4);
        TAP_CHECK(sv.valid() and cl.valid() and sp.valid() and fl.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(hold.close(&g_ci_bounced) == 0);
        TAP_CHECK(hold.close(&g_gate) == 0);
        TAP_CHECK(hold.close(&g_ep) == 0);
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
    }
    void cc_caller(void*) // caps: done@1, lock@2, E(SIGNAL)@3
    {
        char buf[8] = {0};
        g_cc_rc = kos_call(3, buf, 4, sizeof(buf));
        log_put('c');
    }
    void t_call_close_reply()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        TAP_ASK(.workers = 2, .endpoints = 1);
        log_reset();
        g_cc_rc = -99;
        kos::thread::Handle sv;
        kos::thread::Handle cl;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.thread(&sv) and hold.thread(&cl));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        sv = kos::thread::create_caps(cc_server, nullptr, "ccS", 8, scaps, 3);
        TAP_CHECK(sv.valid());
        kos_yield();
        cl = kos::thread::create_caps(cc_caller, nullptr, "ccC", 20, ccaps, 3);
        TAP_CHECK(cl.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(hold.close(&g_ep) == 0);
        int32_t const cc_rc = g_cc_rc;
        TAP_CHECK(cc_rc == -KOS_EPIPE);   // (a) caller woken with EPIPE, not a byte count
        TAP_CHECK(nth('c', 1) < nth('s', 1)); // (b) caller ran before the server proceeded
    }

    // An echo server: records the request it receives and replies "pong!".
    Atomic<int32_t, Order::RELAXED> g_echo_reqn{-99}; // request bytes the server observed
    char g_echo_reqbuf[8];           // request content the server observed
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
            return;
        }
        kos_cap_t const reply = opts.info.reply_cap;
        // An invalid endpoint fails the receive after the reply has woken its caller.
        kos_reply_recv_opts_init(&opts, 0x7fffu, 0, KOS_TIMEOUT_NONE);
        memcpy(buf, "pong!", 5);
        g_frb_rc = kos_reply_recv(reply, buf, kos_call_lens_pack(5, sizeof(buf)), &opts);
        g_frb_server_saw = g_frb_caller_ran.load();
    }
    void frb_caller(void*) // caps: done@1, E(SIGNAL)@2
    {
        char buf[16];
        memcpy(buf, "ping", 4);
        (void)kos_call(2, buf, 4, sizeof(buf));
        g_frb_caller_ran = 1;
    }
    void t_reply_recv_bad_ep_wakes_caller()
    {
        TAP_ASK(.workers = 2, .endpoints = 1);
        g_frb_rc = -99;
        g_frb_caller_ran = 0;
        g_frb_server_saw = 9u;
        kos::thread::Handle sv;
        kos::thread::Handle cl;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.thread(&sv) and hold.thread(&cl));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        sv = irq_spawn(frb_server, nullptr, "frbS", TAP_PRIO_PARKS, scaps, 2);
        TAP_CHECK(sv.valid());
        kos_yield();
        cl = irq_spawn(frb_caller, nullptr, "frbC", 20, ccaps, 2);
        TAP_CHECK(cl.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(hold.close(&g_ep) == 0);
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
    }
    void f4_client(void*) // caps: done@1, E(SIGNAL)@2
    {
        memcpy(g_f4_aliased, "ping", F4_REQ_LEN);
        g_f4_bad = kos_call_timed(2, g_f4_aliased, F4_REQ_LEN, F4_CAP, F4_CALL_US);
        memcpy(g_f4_clean, "ping", F4_REQ_LEN);
        g_f4_good = kos_call_timed(2, g_f4_clean, F4_REQ_LEN, F4_CAP, F4_CALL_US);
    }
    void t_service_survives_client_fault()
    {
        TAP_ASK(.workers = 2, .endpoints = 1);
        g_f4_bad = -99;
        g_f4_good = -99;
        g_f4_served = -99;
        g_f4_dropped = -99;
        memset(g_f4_aliased, 0, sizeof(g_f4_aliased));
        memset(g_f4_clean, 0, sizeof(g_f4_clean));
        kos::thread::Handle sv;
        kos::thread::Handle cl;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.thread(&sv) and hold.thread(&cl));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        sv = irq_spawn(f4_service, nullptr, "f4S", TAP_PRIO_PARKS, scaps, 2);
        TAP_CHECK(sv.valid());
        kos_yield();
        cl = kos::thread::create_caps(f4_client, nullptr, "f4C", 12, ccaps, 2);
        TAP_CHECK(cl.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(hold.close(&g_ep) == 0);
        tap::diag("aliased reply %d, next call %d, served %d, dropped %d",
                  static_cast<int>(g_f4_bad.load()), static_cast<int>(g_f4_good.load()),
                  static_cast<int>(g_f4_served.load()),
                  static_cast<int>(g_f4_dropped.load()));
        TAP_CHECK(g_f4_bad.load() == -KOS_EFAULT);
        TAP_CHECK(g_f4_dropped.load() == 1);
        TAP_CHECK(g_f4_good.load() == static_cast<int32_t>(F4_REPLY_LEN));
        TAP_CHECK(g_f4_served.load() == F4_CALLS);
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // Raise two bound IRQs, consume both bits, then repeat to verify rearming.
    // Use a deadline so a failure reports instead of hanging. These lines are
    // not held by other tests while this test runs.
    constexpr uint32_t FRN_WAIT_US = STALL_TOLERANT_US;
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
            kos_sem_wait(FRN_CH_GO, KOS_TIMEOUT_NONE);
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
    }
    // Claim, attach, bind and arm without raising the line; only the timeout can end the
    // wait.
    Atomic<int32_t, Order::RELAXED> g_irqto_rc{-99};
    void irqto_worker(void*) // caps: done@1, note(FULL)@2
    {
        auto note = kos::Notification::adopt(2);
        if (note.bind() != 0)
        {
            return;
        }
        uint32_t bits = 0;
        g_irqto_rc = note.wait(NOTE_ALL, 20000u, &bits); // 20 ms
    }
    void t_irq_wait_timeout()
    {
        g_irqto_rc = -99;
        kos_cap_t line = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.cap(&line) and hold.cap(&note) and hold.thread(&w));
        TAP_ASK_LINE(&line, .workers = 1, .notifies = 1, .irq_line = IRQ_CTX_LINE);
        note = notify_for_line(line);
        TAP_CHECK(note != KOS_CAP_NONE);
        kos_irq_ack(line); // armed, and still nothing raises it
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {note, CH_FULL}};
        w = irq_spawn(irqto_worker, nullptr, "irqT", 10, caps, 2);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(g_irqto_rc.load() == -KOS_ETIMEDOUT);
    }

    // Test notification delivery to a parked receive, including the resume barrier.
    // A deferred-switch backend must complete that switch before reading notification bits.
    Atomic<int32_t, Order::RELAXED> g_frp_rc{-99};
    Atomic<uint32_t, Order::RELAXED> g_frp_bits{0};
    Atomic<uint32_t, Order::RELAXED> g_frp_mask{0};
    void frp_server(void*) // caps: done@1, E(WAIT)@2, note@3
    {
        char buf[16];
        if (kos_notify_bind(3) != 0)
        {
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
    }
    Atomic<int32_t, Order::RELAXED> g_frp_raised{-99};
    void frp_raiser(void*) // caps: done@1, line(SIGNAL)@2
    {
        g_frp_raised = kos_irq_raise(2);
    }
    void t_reply_recv_notify_park()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        kos_cap_t line = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        kos::thread::Handle sv;
        kos::thread::Handle rz;
        ArmHold hold;
        TAP_HOLD(hold.cap(&line) and hold.cap(&note) and hold.cap(&g_ep) and hold.thread(&sv)
                 and hold.thread(&rz));
        TAP_ASK_LINE(&line, .workers = 2, .endpoints = 1, .notifies = 1, .irq_line = IRQ_CTX_LINE);
        note = notify_for_line(line); // unbadged: this line raises bit 0
        TAP_CHECK(note != KOS_CAP_NONE);
        kos_irq_ack(line); // a claim leaves the line masked
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        g_frp_raised = -99;
        g_frp_rc = -99;
        g_frp_bits = 0;
        g_frp_mask = 0;
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}, {note, CH_FULL}};
        kos_cap_grant rcaps[] = {{g_done, CH_FULL}, {line, KOS_CAP_SIGNAL}};
        sv = irq_spawn(frp_server, nullptr, "frpS", 12, scaps, 3);
        TAP_CHECK(sv.valid());
        rz = kos::thread::create_caps(frp_raiser, nullptr, "frpR", 8, rcaps, 2);
        TAP_CHECK(rz.valid());
        TAP_CHECK(hold.joined());
        uint32_t const mask = g_frp_mask.load();
        TAP_CHECK(g_frp_raised.load() == 0);
        TAP_CHECK(mask != 0);
        TAP_CHECK(g_frp_rc.load() == -KOS_ENOTIFY and g_frp_bits.load() == mask);
    }

    void t_reply_recv_notify()
    {
        // This ordering requires the server to reach its gate before main raises.
        TAP_SKIP_ONE_CORE_ORDER();
        kos_cap_t l1 = KOS_CAP_NONE;
        kos_cap_t l2 = KOS_CAP_NONE;
        kos_cap_t note = KOS_CAP_NONE;
        kos_cap_t b1 = KOS_CAP_NONE;
        kos_cap_t b2 = KOS_CAP_NONE;
        kos::thread::Handle sv;
        ArmHold hold;
        TAP_HOLD(hold.cap(&l1) and hold.cap(&l2) and hold.cap(&note) and hold.cap(&b1)
                 and hold.cap(&b2) and hold.cap(&g_ep) and hold.cap(&g_frn_go)
                 and hold.thread(&sv));
        TAP_ASK_LINE(&l1, .workers = 1, .sems = 1, .endpoints = 1, .notifies = 1, .irqs = 2,
                          .irq_line = IRQ_CTX_LINE);
        // One object, two lines, one bit each: the badge is what tells them apart, and it is
        // seated at the MINT, so main holds two badged copies just long enough to attach.
        TAP_CHECK(irq_claim_await(FRN_LINE2, &l2) == 0);
        TAP_CHECK(kos_notify_create(&note) == 0);
        TAP_CHECK(kos_notify_badge(note, 0u, &b1) == 0);
        TAP_CHECK(kos_notify_badge(note, 1u, &b2) == 0);
        TAP_CHECK(kos_irq_bind_notify(l1, b1) == 0);
        TAP_CHECK(kos_irq_bind_notify(l2, b2) == 0);
        // The attachment holds its own reference.
        TAP_CHECK(hold.close(&b1) == 0);
        TAP_CHECK(hold.close(&b2) == 0);
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        TAP_CHECK(kos_sem_create(0, &g_frn_go) == 0);
        // Arm before the first raise.
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
        sv = irq_spawn(frr_notify_server, nullptr, "frrN", 10, caps, 4);
        TAP_CHECK(sv.valid());
        kos_yield();
        for (int round = 0; round < 3; round++)
        {
            // main rearms, because main is the one holding the line capabilities: the server
            // holds only the object. Both raises must be pending before the wait looks, or
            // the wait parks on the first and the arm measures which ISR won rather than
            // which bits the mask accepted.
            kos_irq_ack(l1);
            kos_irq_ack(l2);
            TAP_CHECK(kos_irq_raise(l1) == 0);
            TAP_CHECK(kos_irq_raise(l2) == 0);
            kos_sem_post(g_frn_go);
        }
        TAP_CHECK(hold.joined());
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
        TAP_ASK(.workers = 2, .endpoints = 1);
        g_frr_served = -99;
        g_frr_ok = -99;
        kos::thread::Handle sv;
        kos::thread::Handle cl;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.thread(&sv) and hold.thread(&cl));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_ep, EP_SIGNAL_ONLY}};
        sv = irq_spawn(frr_server, nullptr, "frrS", TAP_PRIO_PARKS, scaps, 2);
        TAP_CHECK(sv.valid());
        kos_yield();
        cl = kos::thread::create_caps(frr_caller, nullptr, "frrC", 12, ccaps, 2);
        TAP_CHECK(cl.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(hold.close(&g_ep) == 0);
        TAP_CHECK(g_frr_served.load() == 2);
        TAP_CHECK(g_frr_ok.load() == 2);
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // --- Call/reply: the lengths at and around the register fastpath's budget ------------
    // It no longer tells whether the fastpath ran: no production call reads that (ruled).
    // The peers MUST run at equal priority: the fastpath refuses when the caller outranks the
    // server. The reply travels back through the caller's saved trap frame, so the server
    // transforms the request: a reply byte-identical to the request cannot distinguish a
    // correct copy from a buffer the kernel left alone.
    constexpr unsigned char FP_XOR = 0xA5;
    constexpr size_t FP_MAX = 20; // KOS_CALL_REG_BYTES
    Atomic<int32_t, Order::RELAXED> g_fp_reqn{-99};
    Atomic<int32_t, Order::RELAXED> g_fp_rc{-99};
    unsigned char g_fp_rpl[FP_MAX];
    size_t g_fp_send_len = 0;
    size_t g_fp_recv_cap = 0;

    void fp_server(void*) // caps: E(WAIT)@1
    {
        unsigned char buf[64];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 1, 0, KOS_TIMEOUT_NONE);
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
    }
    void fp_caller(void*) // caps: E(SIGNAL)@1
    {
        unsigned char buf[64];
        for (size_t i = 0; i < sizeof(buf); i++)
        {
            buf[i] = static_cast<unsigned char>(i + 1); // no zero byte: a cleared buffer shows
        }
        g_fp_rc = kos_call(1, buf, g_fp_send_len, g_fp_recv_cap);
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
    }
    // One call at the given lengths, the server parked in recv first: 0, or what stopped it.
    int fp_run(size_t send_len, size_t recv_cap)
    {
        g_fp_send_len = send_len;
        g_fp_recv_cap = recv_cap;
        g_fp_reqn = -99;
        g_fp_rc = -99;
        memset(g_fp_rpl, 0, sizeof(g_fp_rpl));
        kos::thread::Handle sv;
        kos::thread::Handle cl;
        ArmHold hold;
        if (not(hold.cap(&g_ep) and hold.thread(&sv) and hold.thread(&cl)))
        {
            return -KOS_EINVAL;
        }
        int const rc = kos_endpoint_create(&g_ep);
        if (rc != 0)
        {
            return rc;
        }
        kos_cap_grant scaps[] = {{g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_ep, EP_SIGNAL_ONLY}};
        sv = irq_spawn(fp_server, nullptr, "fpS", 11, scaps, 1);
        if (not sv.valid())
        {
            return sv.error();
        }
        kos_yield();
        cl = irq_spawn(fp_caller, nullptr, "fpC", 11, ccaps, 1);
        if (not cl.valid())
        {
            return cl.error();
        }
        if (not hold.joined())
        {
            return -KOS_ETIMEDOUT;
        }
        return hold.close(&g_ep);
    }
    bool fp_echoed(size_t n)
    {
        for (size_t i = 0; i < n; i++)
        {
            if (g_fp_rpl[i] != static_cast<unsigned char>((i + 1) ^ FP_XOR))
            {
                return false;
            }
        }
        return true;
    }
    void t_call_reg_fastpath()
    {
        TAP_ASK(.workers = 2, .endpoints = 1);
        // (A) both lengths inside the register budget.
        TAP_CHECK(fp_run(8, 8) == 0);
        int32_t const rc_a = g_fp_rc;
        TAP_CHECK(g_fp_reqn == 8);
        TAP_CHECK(rc_a == 8);
        TAP_CHECK(fp_echoed(8)); // every reply byte, through the caller's saved trap frame

        // (B) the boundary, exactly KOS_CALL_REG_BYTES each way.
        TAP_CHECK(fp_run(FP_MAX, FP_MAX) == 0);
        int32_t const rc_b = g_fp_rc;
        TAP_CHECK(g_fp_reqn == static_cast<int32_t>(FP_MAX));
        TAP_CHECK(rc_b == static_cast<int32_t>(FP_MAX));
        TAP_CHECK(fp_echoed(FP_MAX));

        // (C) a reply capacity ABOVE the budget.
        TAP_CHECK(fp_run(8, 64) == 0);
        int32_t const rc_c = g_fp_rc;
        TAP_CHECK(g_fp_reqn == 8);
        TAP_CHECK(rc_c == 8);
        TAP_CHECK(fp_echoed(8));
    }
#endif // KICKOS_ENABLE_SELFTEST (register-fastpath lengths)

    // --- Call/reply: main calls like any other thread --------------------------------
    // main holds an ordinary thread-pool slot, so a reply capability can name it. Both
    // dispatch sites run against one spawned echo server: (A) the server parked in recv
    // first, (B) main's call first. The server MUST exist before the call either way, or the call is -KOS_EPIPE. It runs at
    // KICKOS_PRIO_MIN: a server main outranks makes main DONATE.
    void rt_witness(void*) {}
    void t_call_from_root()
    {
        TAP_ASK(.workers = 2, .endpoints = 1);
        kos::thread::Handle sv;
        kos::thread::Handle wit;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.thread(&sv) and hold.thread(&wit));
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {0, EP_WAIT_ONLY}};
        char buf[16];

        g_echo_reqn = -99;
        memset(g_echo_reqbuf, 0, sizeof(g_echo_reqbuf));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        scaps[1].source_cap = g_ep;
        sv = irq_spawn(echo_server, nullptr, "rtS", KICKOS_PRIO_MIN, scaps, 2);
        TAP_CHECK(sv.valid());
        // Queued behind the server at its priority, so it runs once the server has parked.
        wit = irq_spawn(rt_witness, nullptr, "rtW", KICKOS_PRIO_MIN, nullptr, 0);
        TAP_CHECK(wit.valid());
        TAP_CHECK(thread_end(&wit) == 0);
        memcpy(buf, "ping", 4);
        int32_t rc = kos_call_timed(g_ep, buf, 4, sizeof(buf), STALL_TOLERANT_US);
        TAP_CHECK(hold.joined());
        TAP_CHECK(kos_sem_wait(g_done, STALL_TOLERANT_US) == 0); // echo_server's
        TAP_CHECK(hold.close(&g_ep) == 0);
        int32_t const echo_reqn_a = g_echo_reqn;
        TAP_CHECK(echo_reqn_a == 4 and memcmp(g_echo_reqbuf, "ping", 4) == 0);
        TAP_CHECK(rc == 5 and memcmp(buf, "pong!", 5) == 0);

        g_echo_reqn = -99;
        memset(g_echo_reqbuf, 0, sizeof(g_echo_reqbuf));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        scaps[1].source_cap = g_ep;
        sv = irq_spawn(echo_server, nullptr, "rtS2", KICKOS_PRIO_MIN, scaps, 2);
        TAP_CHECK(sv.valid());
        memcpy(buf, "ping", 4);
        rc = kos_call_timed(g_ep, buf, 4, sizeof(buf), STALL_TOLERANT_US);
        TAP_CHECK(hold.joined());
        TAP_CHECK(kos_sem_wait(g_done, STALL_TOLERANT_US) == 0); // echo_server's
        TAP_CHECK(hold.close(&g_ep) == 0);
        int32_t const echo_reqn_b = g_echo_reqn;
        TAP_CHECK(echo_reqn_b == 4 and memcmp(g_echo_reqbuf, "ping", 4) == 0);
        TAP_CHECK(rc == 5 and memcmp(buf, "pong!", 5) == 0);
    }

    // Spawns `server` at TAP_PRIO_PARKS, lets it park in recv, then `caller` above it, and
    // joins both: the one-call shape of the arms below. Both take E@1.
    bool serve_one_call(void (*server)(void*), void (*caller)(void*), ArmHold& hold,
                        kos::thread::Handle& sv, kos::thread::Handle& cl)
    {
        kos_cap_grant scaps[] = {{g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_ep, EP_SIGNAL_ONLY}};
        sv = irq_spawn(server, nullptr, "srv", TAP_PRIO_PARKS, scaps, 1);
        if (not sv.valid())
        {
            return false;
        }
        kos_yield();
        cl = irq_spawn(caller, nullptr, "cli", 12, ccaps, 1);
        return cl.valid() and hold.joined();
    }

    // --- Call/reply: reply + request truncation (datagram clamp, not an error) ---
    // One call exercises BOTH clamps: the caller sends 8 bytes into a server recv buffer of
    // 3 (request truncated to 3), and the server replies 8 bytes into a caller recv_cap of 3
    // (reply truncated to 3). Neither is an error: the byte counts just clamp.
    Atomic<int32_t, Order::RELAXED> g_trunc_reqn{-99}; // request bytes the server saw (its buffer < send_len)
    char g_trunc_reqbuf[4];
    Atomic<int32_t, Order::RELAXED> g_trunc_rc{-99}; // caller's kos_call return (clamped to recv_cap)
    char g_trunc_rplbuf[4];
    void trunc_server(void*) // caps: E(WAIT)@1
    {
        char buf[3]; // smaller than the 8-byte request
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 1, 0, KOS_TIMEOUT_NONE);
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
    }
    void trunc_caller(void*) // caps: E(SIGNAL)@1
    {
        char buf[8];
        memcpy(buf, "ABCDEFGH", 8);
        g_trunc_rc = kos_call(1, buf, 8, 3); // recv_cap = 3 -> the reply clamps into it
        if (g_trunc_rc > 0)
        {
            size_t k = static_cast<size_t>(g_trunc_rc);
            if (k > sizeof(g_trunc_rplbuf)) { k = sizeof(g_trunc_rplbuf); }
            memcpy(g_trunc_rplbuf, buf, k);
        }
    }
    void t_call_truncation()
    {
        TAP_ASK(.workers = 2, .endpoints = 1);
        g_trunc_reqn = -99; g_trunc_rc = -99;
        memset(g_trunc_reqbuf, 0, sizeof(g_trunc_reqbuf));
        memset(g_trunc_rplbuf, 0, sizeof(g_trunc_rplbuf));
        kos::thread::Handle sv;
        kos::thread::Handle cl;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.thread(&sv) and hold.thread(&cl));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        TAP_CHECK(serve_one_call(trunc_server, trunc_caller, hold, sv, cl));
        TAP_CHECK(hold.close(&g_ep) == 0);
        int32_t const trunc_reqn = g_trunc_reqn;
        int32_t const trunc_rc = g_trunc_rc;
        TAP_CHECK(trunc_reqn == 3 and memcmp(g_trunc_reqbuf, "ABC", 3) == 0);
        TAP_CHECK(trunc_rc == 3 and memcmp(g_trunc_rplbuf, "123", 3) == 0);
    }

    // --- Call/reply: server dies pre-pop -> caller refused (recv_holders -> 0) ---
    // The caller parks in SEND_WAIT (no receiver has popped it yet). MAIN holds the sole
    // WAIT cap; closing it drives recv_holders to 0, which drains send_waiters and refuses
    // the parked call.
    Atomic<int32_t, Order::RELAXED> g_pp_callrc{-99};
    void pp_caller(void*) // caps: E(SIGNAL)@1
    {
        char buf[8] = {0};
        g_pp_callrc = kos_call(1, buf, 4, sizeof(buf)); // parks SEND_WAIT; woken refused
    }
    void t_call_prepop_death()
    {
        TAP_ASK(.workers = 1, .endpoints = 1);
        g_pp_callrc = -99;
        kos::thread::Handle cl;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.thread(&cl));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        // Caller gets SIGNAL only; MAIN is the sole WAIT holder. No server ever recvs.
        kos_cap_grant ccaps[] = {{g_ep, EP_SIGNAL_ONLY}};
        cl = irq_spawn(pp_caller, nullptr, "ppC", TAP_PRIO_PARKS, ccaps, 1);
        TAP_CHECK(cl.valid());
        kos_yield();
        TAP_CHECK(hold.close(&g_ep) == 0);  // last WAIT cap -> recv_holders 0 -> refuse the call
        TAP_CHECK(hold.joined());
        int32_t const pp_callrc = g_pp_callrc;
        TAP_CHECK(pp_callrc == -KOS_ECONNREFUSED);
    }

    // --- Call/reply: donation ordering ------------------------------------------
    // low(8) server, high(20) caller, medium(12) spoiler, one core. With donation the reply reaches
    // the caller ('c') before the spoiler ('m').
    Atomic<int32_t, Order::RELAXED> g_don_rc{-99};
    char g_don_rpl[8];
    void don_server(void*) // caps: done@1, lock@2, E(WAIT)@3, mutex@4, go@5
    {
        kos_mutex_lock(4);
        char buf[16];
        struct kos_recv_info info = {0, KOS_CAP_NONE};
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 3, 0, KOS_TIMEOUT_NONE);
        kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)), &opts); // parks first (no senders); D1-boosted at the call
        info = opts.info;
        log_put('a');
        kos_sem_post(5); // the spoiler is ready from here, under the boost
        kos_mutex_unlock(4); // the recompute under test: the reply donor must hold the boost
        log_put('r');
        if (info.reply_cap != KOS_CAP_NONE)
        {
            char rpl[8];
            memcpy(rpl, "pong!", 5);
            kos_reply(info.reply_cap, rpl, 5);
        }
    }
    void don_caller(void*) // caps: done@1, lock@2, E(SIGNAL)@3, go@4
    {
        char buf[8];
        kos_sem_wait(4, KOS_TIMEOUT_NONE);
        log_put('k');
        memcpy(buf, "req", 3);
        g_don_rc = kos_call(3, buf, 3, sizeof(buf));
        if (g_don_rc > 0)
        {
            size_t k = static_cast<size_t>(g_don_rc);
            if (k > sizeof(g_don_rpl)) { k = sizeof(g_don_rpl); }
            memcpy(g_don_rpl, buf, k);
        }
        log_put('c');
    }
    void don_spoiler(void*) // caps: done@1, lock@2, go@3 (medium prio)
    {
        kos_sem_wait(3, KOS_TIMEOUT_NONE);
        log_put('m');
    }
    // Spawns the donation trio and starts the caller once every party has parked. `mutex` is
    // the one the server holds across its receive.
    bool don_run(ArmHold& hold, kos_cap_t go, kos_cap_t mutex, kos::thread::Handle& sv,
                 kos::thread::Handle& cl, kos::thread::Handle& sp)
    {
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY},
                                 {mutex, CH_MTX}, {go, CH_FULL}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY},
                                 {go, CH_FULL}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {go, CH_FULL}};
        sv = kos::thread::create_caps(don_server, nullptr, "dnS", 8, scaps, 5);
        cl = kos::thread::create_caps(don_caller, nullptr, "dnC", 20, ccaps, 4);
        sp = kos::thread::create_caps(don_spoiler, nullptr, "dnM", 12, mcaps, 3);
        if (not sv.valid() or not cl.valid() or not sp.valid())
        {
            return false;
        }
        kos_yield();
        kos_sem_post(go);
        return hold.joined();
    }
    // Unlock an unrelated mutex during an IPC transaction and verify the server keeps its
    // donation. If its priority falls to base(8) at the unlock, the ready spoiler(12) runs
    // first.
    void t_call_donation_hold()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        TAP_ASK(.workers = 3, .sems = 1, .mutexes = 1, .endpoints = 1);
        log_reset();
        g_don_rc = -99;
        memset(g_don_rpl, 0, sizeof(g_don_rpl));
        kos_cap_t go = KOS_CAP_NONE;
        kos_cap_t m = KOS_CAP_NONE;
        kos::thread::Handle sv;
        kos::thread::Handle cl;
        kos::thread::Handle sp;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.cap(&go) and hold.cap(&m) and hold.thread(&sv)
                 and hold.thread(&cl) and hold.thread(&sp));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        TAP_CHECK(kos_sem_create(0, &go) == 0);
        TAP_CHECK(kos_mutex_create(&m) == 0);
        TAP_CHECK(don_run(hold, go, m, sv, cl, sp));
        TAP_CHECK(hold.close(&g_ep) == 0);
        TAP_CHECK(hold.close(&m) == 0);
        int32_t const dh_rc = g_don_rc;
        TAP_CHECK(dh_rc == 5 and memcmp(g_don_rpl, "pong!", 5) == 0);
        TAP_CHECK(count('a') == 1 and count('r') == 1 and count('c') == 1 and count('m') == 1);
        TAP_CHECK(nth('r', 1) < nth('c', 1)); // reply delivered: the caller ran after the server replied
        // The unlock's recompute did NOT deflate the server, and the reply reached the caller
        // before the spoiler ran.
        TAP_CHECK(nth('c', 1) < nth('m', 1));
    }

    // Same again for the OTHER mint site: the caller parks in SEND_WAIT first and the
    // server's recv pops it, so the reply cap is minted from the server's own syscall. The
    // two sites link the donor independently.
    //
    // Staged by handoff on one semaphore, never by a duration: the first post makes the slowpath
    // call, the second readies the spoiler under the boost.
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
    }
    void t_call_donation_slow()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        TAP_ASK(.workers = 3, .sems = 1, .mutexes = 1, .endpoints = 1);
        log_reset();
        g_don_rc = -99;
        kos_cap_t go = KOS_CAP_NONE;
        kos_cap_t m = KOS_CAP_NONE;
        kos::thread::Handle sv;
        kos::thread::Handle cl;
        kos::thread::Handle sp;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.cap(&go) and hold.cap(&m) and hold.thread(&sv)
                 and hold.thread(&cl) and hold.thread(&sp));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        TAP_CHECK(kos_mutex_create(&m) == 0);
        TAP_CHECK(kos_sem_create(0, &go) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY},
                                 {m, CH_MTX}, {go, CH_FULL}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY},
                                 {go, CH_FULL}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {go, CH_FULL}};
        // The two waiters first: the server must find both parked when it first runs.
        cl = kos::thread::create_caps(don_caller, nullptr, "dsC", 20, ccaps, 4);
        sp = kos::thread::create_caps(don_spoiler, nullptr, "dsM", 12, mcaps, 3);
        sv = kos::thread::create_caps(ds_server, nullptr, "dsS", 8, scaps, 5);
        TAP_CHECK(sv.valid() and cl.valid() and sp.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(hold.close(&g_ep) == 0);
        TAP_CHECK(hold.close(&m) == 0);
        int32_t const dh_rc = g_don_rc;
        TAP_CHECK(dh_rc == 5);
        TAP_CHECK(count('s') == 1 and count('k') == 1 and count('a') == 1 and count('r') == 1
                  and count('m') == 1);
        TAP_CHECK(nth('s', 1) < nth('k', 1) and nth('k', 1) < nth('a', 1)); // called while awake
        TAP_CHECK(nth('r', 1) < nth('m', 1));
    }

    // Same shape, but the donor is a caller parked in SEND_WAIT rather than a reply cap:
    // the server takes and replies to a first call, so no reply cap is live, and the
    // caller's SECOND call arrives while the server is awake (slowpath -> D2 boost).
    Atomic<int, Order::RELAXED> g_dp_call2{0};
    Atomic<int, Order::RELAXED> g_dp_parked{-1};
    void dp_server(void*) // caps: done@1, lock@2, E(WAIT)@3, mutex@4, go@5
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
        g_dp_parked = g_dp_call2.load();
        log_put('a');
        kos_sem_post(5);         // the spoiler is ready from here
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
    }
    void dp_caller(void*) // caps: done@1, lock@2, E(SIGNAL)@3, go@4
    {
        char buf[8];
        kos_sem_wait(4, KOS_TIMEOUT_NONE);
        memcpy(buf, "a", 1);
        int32_t const r1 = kos_call(3, buf, 1, sizeof(buf));
        memcpy(buf, "b", 1);
        g_dp_call2 = 1;
        int32_t const r2 = kos_call(3, buf, 1, sizeof(buf)); // server is awake -> slowpath
        g_don_rc = r1 + r2;
        log_put('c');
    }
    void t_call_donation_pending()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        TAP_ASK(.workers = 3, .sems = 1, .mutexes = 1, .endpoints = 1);
        log_reset();
        g_dp_call2 = 0;
        g_dp_parked = -1;
        g_don_rc = -99;
        kos_cap_t go = KOS_CAP_NONE;
        kos_cap_t m = KOS_CAP_NONE;
        kos::thread::Handle sv;
        kos::thread::Handle cl;
        kos::thread::Handle sp;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.cap(&go) and hold.cap(&m) and hold.thread(&sv)
                 and hold.thread(&cl) and hold.thread(&sp));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        TAP_CHECK(kos_sem_create(0, &go) == 0);
        TAP_CHECK(kos_mutex_create(&m) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY},
                                 {m, CH_MTX}, {go, CH_FULL}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY},
                                 {go, CH_FULL}};
        kos_cap_grant mcaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {go, CH_FULL}};
        sv = kos::thread::create_caps(dp_server, nullptr, "dpS", 8, scaps, 5);
        cl = kos::thread::create_caps(dp_caller, nullptr, "dpC", 20, ccaps, 4);
        sp = kos::thread::create_caps(don_spoiler, nullptr, "dpM", 12, mcaps, 3);
        TAP_CHECK(sv.valid() and cl.valid() and sp.valid());
        kos_yield();
        kos_sem_post(go);
        TAP_CHECK(hold.joined());
        TAP_CHECK(g_dp_parked.load() == 1);
        TAP_CHECK(hold.close(&g_ep) == 0);
        TAP_CHECK(hold.close(&m) == 0);
        int32_t const dh_rc = g_don_rc;
        TAP_CHECK(dh_rc == 2);
        TAP_CHECK(count('a') == 1 and count('r') == 1 and count('m') == 1);
        TAP_CHECK(nth('r', 1) < nth('m', 1));
    }

#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
    // --- Bound-check: a recv/send pointer outside the caller's regions -> -KOS_EFAULT
    // The write-oracle / cross-domain-read is closed the same way as the console
    // buffer: an unprivileged caller cannot launder an un-owned page through IPC.
    // One per g_unreached subject.
    Atomic<int32_t, Order::RELAXED> g_ep_badrecv_rc[UNREACHED];
    Atomic<int32_t, Order::RELAXED> g_ep_badsend_rc[UNREACHED];
    Atomic<int32_t, Order::RELAXED> g_ep_badopts_rc[UNREACHED];
    void ep_bound_worker(void*) // caps: done@1, E@2 (unpriv)
    {
        for (int i = 0; i < UNREACHED; i++)
        {
            void* const bad = g_unreached[i];
            if (bad == nullptr)
            {
                continue;
            }
            struct kos_reply_recv_opts opts;
            kos_reply_recv_opts_init(&opts, 2, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
            g_ep_badrecv_rc[i] = kos_reply_recv(KOS_CAP_NONE, bad, kos_call_lens_pack(0, 8), &opts); // write oracle -> -KOS_EFAULT
            g_ep_badsend_rc[i] = kos_send(2, static_cast<char const*>(bad), 8); // cross-domain read -> -KOS_EFAULT
            // The opts struct is IN-OUT, and both subjects are word-aligned, so this clears
            // the alignment gate above and lands on the readable+writable check. The message
            // buffer is a valid stack local: the refusal is about opts alone.
            char obuf[8];
            g_ep_badopts_rc[i] =
                kos_reply_recv(KOS_CAP_NONE, obuf, kos_call_lens_pack(0, sizeof(obuf)),
                               static_cast<struct kos_reply_recv_opts*>(bad));
        }
    }
    void t_endpoint_bound()
    {
        TAP_CHECK(unreached_ready());
        TAP_ASK(.workers = 1, .endpoints = 1);
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.thread(&w));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        for (int i = 0; i < UNREACHED; i++)
        {
            g_ep_badrecv_rc[i] = -99;
            g_ep_badsend_rc[i] = -99;
            g_ep_badopts_rc[i] = -99;
        }
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_ep, CH_FULL}};
        w = kos::thread::create_caps(ep_bound_worker, nullptr, "epbn", 12, caps, 2,
                                     KOS_POLICY_FIFO, 0, /*privileged=*/false);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(hold.close(&g_ep) == 0);
        for (int i = 0; i < UNREACHED; i++)
        {
            if (g_unreached[i] != nullptr)
            {
                TAP_CHECK(g_ep_badrecv_rc[i].load() == -KOS_EFAULT); // rejected, never parked
                TAP_CHECK(g_ep_badsend_rc[i].load() == -KOS_EFAULT); // rejected, never parked
                TAP_CHECK(g_ep_badopts_rc[i].load() == -KOS_EFAULT); // no deadline read
            }
        }
        unreached_partial();
    }
#endif

    // --- Cross-domain rendezvous under enforcement -------------------------------
    // main and an UNPRIVILEGED worker in a DIFFERENT memory domain rendezvous both ways: the
    // arriving side's kernel copy lands in the parked peer's domain, not the arriver's loaded
    // regions. The worker's payload buffer lives in its own granted domain region, and the
    // clean endpoint free at the end is what validates the delegation accounting.
    // A round trip and not a pair of reports: the worker's memory grant makes it a task of
    // its own, so an app global it writes is its own copy. Its verdict comes back over the
    // endpoint.
    void xd_worker(void* arg) // caps: E(FULL)@1; arg = domain buffer
    {
        char* b = static_cast<char*>(arg);
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, 1, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
        int32_t const n = kos_reply_recv(KOS_CAP_NONE, b, kos_call_lens_pack(0, 8), &opts);
        int32_t verdict = n;
        for (int i = 0; i < 8; i++)
        {
            if (b[i] != static_cast<char>('a' + i))
            {
                verdict = -1;
            }
        }
        (void)kos_send_timed(1, &verdict, sizeof(verdict), STALL_TOLERANT_US);
    }
    constexpr RamAsk XD_RAM = {256};
    void t_endpoint_crossdomain()
    {
        TAP_ASK(.workers = 1, .endpoints = 1);
        void* const wbuf = st_ram<XD_RAM, 0>();
        TAP_CHECK(wbuf != nullptr);
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.thread(&w));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        kos_cap_grant wcaps[] = {{g_ep, CH_FULL}};
        w = kos::thread::create_caps(xd_worker, wbuf, "xdW", 12, wcaps, 1,
                                     KOS_POLICY_FIFO, 0, /*privileged=*/false, wbuf, 256);
        TAP_CHECK(w.valid());
        char out[8];
        for (int i = 0; i < 8; i++)
        {
            out[i] = static_cast<char>('a' + i);
        }
        int32_t const sent = kos_send_timed(g_ep, out, 8, STALL_TOLERANT_US);
        int32_t verdict = -99;
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, g_ep, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
        int32_t const got = kos_reply_recv(KOS_CAP_NONE, &verdict, kos_call_lens_pack(0, sizeof(verdict)), &opts);
        TAP_CHECK(hold.joined());
        int const closed = hold.close(&g_ep); // both delegated caps torn down -> freed
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
    // for cap_gen_reuse to reach EMFILE.
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

    // Untyped, so it can go on to the endpoint and notification budgets: a child of main's
    // task fills its own table out of the budgets main's census leaves.
    int fill_one_cap(kos_cap_t* out)
    {
        bool is_sem = false;
        int rc = fill_one_cap_typed(out, &is_sem);
        if (rc == -KOS_ENOMEM or rc == -KOS_EAGAIN)
        {
            rc = kos_endpoint_create(out);
        }
        if (rc == -KOS_ENOMEM or rc == -KOS_EAGAIN)
        {
            rc = kos_notify_create(out);
        }
        return rc;
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

#if KICKOS_HAVE_ASPACE
    long mem_census(uint32_t which)
    {
        uint32_t n = 0;
        if (kos_mem_count(which, &n) != 0)
        {
            return -1;
        }
        return static_cast<long>(n);
    }
    long frames_census() { return mem_census(KOS_MEM_FRAMES_FREE); }
    long spaces_census() { return mem_census(KOS_MEM_SPACES_HELD); }
    long ranges_census() { return mem_census(KOS_MEM_RANGES_FREE); }
#endif

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
        ArmHold hold;
        TAP_HOLD(hold.owned(&fresh));
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
            TAP_CHECK(kos_sem_wait(fresh, 0) == 0);
        }
        else
        {
            TAP_CHECK(kos_mutex_lock(stale) == -KOS_EBADF);
            TAP_CHECK(kos_mutex_lock(fresh) == 0);
            TAP_CHECK(kos_mutex_unlock(fresh) == 0);
        }
        TAP_CHECK(kos_handle_close(stale) == -KOS_EBADF);
        TAP_CHECK(hold.close(&fresh) == 0);
        for (int i = 0; i < last; i++)
        {
            TAP_CHECK(fill.close(i) == 0);
        }
    }

    // --- Per-task table width, and the inbound reply bound ------------------------
    //
    // No new file-scope state below: every worker reports through the shared event log, and
    // a static here would come straight out of the 16 KiB boards' user arena.

    // Marks one '#' per capability it got: the width it was seated with, minus the reserved
    // plane, minus its two grants.
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
    }

    // --- main's table and its child's are both KICKOS_CAP_CHILD_WIDTH wide ----------------
    void t_cap_child_width()
    {
        {
            // The init's delegations land from KOS_SPAWN_DELEGATED_CAP0; g_lock and g_done are
            // main's two.
            uint32_t const seated = KOS_SPAWN_DELEGATED_CAP0 + g_self->cap_grant_count;
            TableFill fill;
            tap::diag("main's table: %u seated, 2 shared, %d filled to %d, of %u",
                      static_cast<unsigned>(seated), fill.n, fill.stop,
                      static_cast<unsigned>(KICKOS_CAP_CHILD_WIDTH));
            TAP_CHECK(fill.stop == -KOS_EMFILE);
            TAP_CHECK(seated + 2u + static_cast<uint32_t>(fill.n) == KICKOS_CAP_CHILD_WIDTH);
        }
        TAP_ASK(.workers = 1);
        log_reset();
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.thread(&w));
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        w = kos::thread::create_caps(width_child, nullptr, "cw", 10, caps, 2);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(count('E') == 1);
        TAP_CHECK(count('#') == KICKOS_CAP_CHILD_WIDTH - KOS_CAP_FIRST_DYNAMIC - 2);
    }

    // Holds A's reply capability across a SECOND recv, so B's call meets the bound. main's
    // first plain send is what unparks that second recv; the reply to A follows it, and that
    // is what lifts the bound for B's retry. main's second plain send ends the run.
    void rb_server(void* arg) // caps: done@1, lock@2, E(WAIT)@3, go@4
    {
        char b[8];
        if (arg != nullptr)
        {
            kos_sem_wait(4, KOS_TIMEOUT_NONE);
        }
        struct kos_recv_info first = {0, KOS_CAP_NONE};
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, CH_AUX, 0, STALL_TOLERANT_US);
        kos_reply_recv(KOS_CAP_NONE, b, kos_call_lens_pack(0, sizeof(b)), &opts);
        first = opts.info;
        log_put('1');
        struct kos_recv_info wake = {0, KOS_CAP_NONE};
        struct kos_reply_recv_opts opts2;
        kos_reply_recv_opts_init(&opts2, CH_AUX, 0, STALL_TOLERANT_US);
        kos_reply_recv(KOS_CAP_NONE, b, kos_call_lens_pack(0, sizeof(b)), &opts2);
        wake = opts2.info;
        kos_reply(first.reply_cap, "r", 1); // A's reply capability stops existing here
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
            kos_reply_recv_opts_init(&opts3, CH_AUX, 0, STALL_TOLERANT_US);
            int32_t const n = kos_reply_recv(KOS_CAP_NONE, b, kos_call_lens_pack(0, sizeof(b)), &opts3);
            info = opts3.info;
            if (n < 0)
            {
                return;
            }
            if (info.reply_cap == KOS_CAP_NONE)
            {
                plains = plains + 1;
                continue;
            }
            kos_reply(info.reply_cap, "r", 1);
        }
    }
    void rb_caller_a(void*) // caps: done@1, lock@2, E(SIGNAL)@3, go@4
    {
        char b[8] = {0};
        kos_sem_wait(4, KOS_TIMEOUT_NONE);
        if (kos_call_timed(CH_AUX, b, 4, sizeof(b), STALL_TOLERANT_US) == 1)
        {
            log_put('A');
        }
    }
    void rb_caller_b(void*) // caps: done@1, lock@2, E(SIGNAL)@3, go@4
    {
        char b[8] = {0};
        kos_sem_wait(4, KOS_TIMEOUT_NONE);
        if (kos_call_timed(CH_AUX, b, 4, sizeof(b), STALL_TOLERANT_US) == -KOS_EMFILE)
        {
            log_put('E'); // refused against the SERVER's reply bound, not against our table
        }
        kos_sem_wait(4, KOS_TIMEOUT_NONE); // main's go once A has been replied to
        if (kos_call_timed(CH_AUX, b, 4, sizeof(b), STALL_TOLERANT_US) == 1)
        {
            log_put('K'); // and admitted once A's reply capability was consumed
        }
    }

    // `slow` decides WHICH probe refuses B. Without it the server parks in recv before either
    // caller runs, so B meets endpoint_call's fastpath probe; with it both callers park in
    // CALL_SEND_WAIT before the server's first recv, so the refusal comes back through the
    // recv-side scan.
    void reply_bound_arm(bool slow)
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
        TAP_ASK(.workers = 3, .sems = 1, .endpoints = 1);
        log_reset();
        kos_cap_t go = KOS_CAP_NONE;
        kos::thread::Handle sv;
        kos::thread::Handle ca;
        kos::thread::Handle cb;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.cap(&go) and hold.thread(&sv) and hold.thread(&ca)
                 and hold.thread(&cb));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        TAP_CHECK(kos_sem_create(0, &go) == 0);
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY},
                                 {go, CH_FULL}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY},
                                 {go, CH_FULL}};
        void* gated = nullptr;
        if (slow)
        {
            gated = &g_ep;
        }
        sv = kos::thread::create_caps(rb_server, gated, "rbS", 8, scaps, 4);
        ca = kos::thread::create_caps(rb_caller_a, nullptr, "rbA", 20, ccaps, 4);
        cb = kos::thread::create_caps(rb_caller_b, nullptr, "rbB", 12, ccaps, 4);
        TAP_CHECK(sv.valid() and ca.valid() and cb.valid());
        kos_yield();
        TAP_CHECK(kos_sem_post(go) == 0); // A calls
        TAP_CHECK(kos_sem_post(go) == 0); // B calls
        if (slow)
        {
            TAP_CHECK(kos_sem_post(go) == 0); // the server takes A, and its next recv refuses B
        }
        char plain[4] = {0};
        // Unparks the second recv, so the server replies to A.
        TAP_CHECK(kos_send_timed(g_ep, plain, 4, STALL_TOLERANT_US) == 4);
        TAP_CHECK(kos_sem_post(go) == 0); // B retries
        TAP_CHECK(kos_send_timed(g_ep, plain, 4, STALL_TOLERANT_US) == 4); // ends the run
        TAP_CHECK(hold.joined());
        TAP_CHECK(hold.close(&g_ep) == 0);
        TAP_CHECK(count('1') == 1);           // the server took A's call first
        TAP_CHECK(count('E') == 1);           // B refused while A's reply capability was live
        TAP_CHECK(count('K') == 1);           // and admitted after it was consumed
        TAP_CHECK(count('A') == 1);           // A was never crowded out by B
        TAP_CHECK(nth('E', 1) < nth('2', 1)); // the refusal preceded the reply that lifted it
        TAP_CHECK(nth('2', 1) < nth('K', 1));
    }

    // --- the reply bound, met at endpoint_call's fastpath probe ---------------------------
    void t_cap_reply_bound_fast()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        reply_bound_arm(false);
    }

    // --- the same bound, delivered through the recv-side scan of parked callers -----------
    void t_cap_reply_bound_slow()
    {
        TAP_SKIP_ONE_CORE_ORDER();
        reply_bound_arm(true);
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
    }
    void rp_caller(void*) // caps: done@1, lock@2, E(SIGNAL)@3, go@4
    {
        char b[8] = {0};
        kos_sem_wait(4, KOS_TIMEOUT_NONE);
        int32_t const rc = kos_call_timed(CH_AUX, b, 4, sizeof(b), STALL_TOLERANT_US);
        if (rc == -KOS_EPIPE)
        {
            log_put('P');
        }
        if (rc == 1)
        {
            log_put('B');
        }
    }

    // The server parks in recv first; each post of `go` releases one waiter's call.
    bool rp_spawn(void (*server)(void*), void* arg, kos_cap_t go, kos::thread::Handle& sv,
                  kos::thread::Handle& cl)
    {
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY},
                                 {go, CH_FULL}};
        sv = irq_spawn(server, arg, "rpS", 8, scaps, 3);
        cl = irq_spawn(rp_caller, nullptr, "rpA", 20, ccaps, 4);
        if (not sv.valid() or not cl.valid())
        {
            return false;
        }
        kos_yield();
        return kos_sem_post(go) == 0;
    }

    // --- a reply cap consumed by CLOSE still admits the next caller -----------------------
    void t_cap_reply_release_close()
    {
        TAP_ASK(.workers = 3, .sems = 1, .endpoints = 1);
        log_reset();
        kos_cap_t go = KOS_CAP_NONE;
        kos::thread::Handle sv;
        kos::thread::Handle ca;
        kos::thread::Handle cb;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.cap(&go) and hold.thread(&sv) and hold.thread(&ca)
                 and hold.thread(&cb));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        TAP_CHECK(kos_sem_create(0, &go) == 0);
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}, {g_ep, EP_SIGNAL_ONLY},
                                 {go, CH_FULL}};
        cb = irq_spawn(rp_caller, nullptr, "rpB", 12, ccaps, 4);
        TAP_CHECK(cb.valid());
        // A's call: the server closes its reply capability and A wakes -KOS_EPIPE.
        TAP_CHECK(rp_spawn(rp_server, &g_ep, go, sv, ca));
        TAP_CHECK(kos_sem_post(go) == 0); // B's call, after the close
        char plain[4] = {0};
        TAP_CHECK(kos_send_timed(g_ep, plain, 4, STALL_TOLERANT_US) == 4);
        TAP_CHECK(hold.joined());
        TAP_CHECK(hold.close(&g_ep) == 0);
        TAP_CHECK(count('c') == 1 and count('P') == 1); // A's cap closed, A woken -KOS_EPIPE
        TAP_CHECK(count('K') == 1 and count('B') == 1); // and B admitted behind it
        TAP_CHECK(nth('c', 1) < nth('K', 1));
    }

    // Takes a call and EXITS holding the reply capability: the teardown sweep is what has to
    // account it, or the slot's next occupant refuses its own first caller.
    void rr_dying_server(void*) // caps: done@1, lock@2, E(WAIT)@3
    {
        char b[8];
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, CH_AUX, 0, STALL_TOLERANT_US);
        kos_reply_recv(KOS_CAP_NONE, b, kos_call_lens_pack(0, sizeof(b)), &opts);
        log_put('d');
    }

    // --- a slot reclaimed from a server that died mid-call admits its next caller ---------
    void t_cap_reply_slot_reuse()
    {
        TAP_ASK(.workers = 2, .sems = 1, .endpoints = 1);
        log_reset();
        kos_cap_t go = KOS_CAP_NONE;
        kos::thread::Handle sv;
        kos::thread::Handle cl;
        ArmHold hold;
        TAP_HOLD(hold.cap(&g_ep) and hold.cap(&go) and hold.thread(&sv) and hold.thread(&cl));
        TAP_CHECK(kos_endpoint_create(&g_ep) == 0);
        TAP_CHECK(kos_sem_create(0, &go) == 0);
        TAP_CHECK(rp_spawn(rr_dying_server, nullptr, go, sv, cl));
        TAP_CHECK(hold.joined());
        TAP_CHECK(count('d') == 1 and count('P') == 1); // died holding a live reply cap

        // Both are gone, so these two reclaim their slots.
        TAP_CHECK(rp_spawn(rp_server, nullptr, go, sv, cl));
        char plain[4] = {0};
        TAP_CHECK(kos_send_timed(g_ep, plain, 4, STALL_TOLERANT_US) == 4);
        TAP_CHECK(hold.joined());
        TAP_CHECK(hold.close(&g_ep) == 0);
        TAP_CHECK(count('K') == 1 and count('B') == 1); // the next occupant admitted its first
    }

    // --- console_publish needs AUTH_CONSOLE; a bad cap is rejected with no side effect ---
    int g_pub_rc = -99;
    void pub_denied_worker(void*) // caps: done@1
    {
        // Unprivileged caller: rejected before any console state change, so this never
        // actually hands over the console. The rest of the suite keeps printing.
        g_pub_rc = kos_console_publish(1, KOS_TASK_NONE);
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
        TAP_ASK(.workers = 1);
        g_pub_rc = -99;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.thread(&w));
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        w = kos::thread::create_caps(pub_denied_worker, nullptr, "pubden", 10, caps, 1);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(g_pub_rc == -KOS_EPERM);
    }

    // --- a console published through HANDOUT for a task, given back when that task ends -----
    // main's own stdout is the endpoint it publishes, so from each publish until the task's end
    // main writes nothing: a TAP line there would park main on a console nobody receives on.
    // Each arm's own lines, and every line after, reach the wire only once the kernel console is
    // back, which is what the stream gate counts.
    constexpr uint32_t CON_KEPT = KOS_CAP_SIGNAL | KOS_CAP_TRANSFER | KOS_CAP_HANDOUT;
    constexpr uint32_t CON_PARK_US = 1000;
    kos_cap_t g_con_ep = KOS_CAP_NONE;
    // Takes one message and exits.
    void con_driver(void*) // caps: the console endpoint@1, WAIT only
    {
        char got[4];
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, KOS_SPAWN_DELEGATED_CAP0, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
        (void)kos_reply_recv(KOS_CAP_NONE, got, kos_call_lens_pack(0, sizeof(got)), &o);
    }

    // The driver task's entry: it holds no right on the console and lives until main lets it go.
    void con_keeper(void*) // caps: hold@1
    {
        (void)kos_sem_wait(KOS_SPAWN_DELEGATED_CAP0, KOS_TIMEOUT_NONE);
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
        (void)kos_sem_wait(KOS_SPAWN_DELEGATED_CAP0, KOS_TIMEOUT_NONE);
        if (kos_send(KOS_CAP_STDOUT, &probe, 1) == -KOS_ETIMEDOUT)
        {
            seen |= NB_SENT;
        }
        kickos::WriteResult const tried = kickos::stdout_write(&probe, 1);
        if (tried.sent == 0u and tried.error == -KOS_ETIMEDOUT)
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
        kos::thread::Handle probe;
        ArmHold hold;
        TAP_HOLD(hold.task(&nb_task) and hold.cap(&go) and hold.cap(&back)
                 and hold.thread(&probe));
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &nb_task) == 0);
        TAP_CHECK(kos_sem_create(0, &go) == 0 and kos_sem_create(0, &back) == 0);
        kos_cap_grant const caps[] = {{go, CH_FULL}, {back, CH_FULL}};
        probe = kos::thread::create_caps(con_nb_probe, nullptr, "connb", 10, caps, 2,
                                         KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                         nb_task);
        TAP_CHECK(probe.valid());
        TAP_CHECK(kos_sem_wait(back, STALL_TOLERANT_US) == 0);
        char const p = 'y';
        g_nb.main_blocking = not con_nonblocking();
        g_nb.main_sent = kos_send_timed(KOS_CAP_STDOUT, &p, 1, CON_PARK_US);
        (void)kos_sem_post(go);
        int seen = 0;
        TAP_CHECK(hold.joined());
        TAP_CHECK(kos_task_exit_status(nb_task, &seen) == 0);
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
    }

    void t_console_publish_handout()
    {
        char const probe = '\0';
        if (kos_send_timed(KOS_CAP_STDOUT, &probe, 0, CON_PARK_US) != -KOS_EBADF)
        {
            tap::skip("this image already publishes a console");
            return;
        }
        TAP_ASK(.workers = 4, .tasks = 2, .sems = 3, .endpoints = 1);
        kos_cap_t bare = KOS_CAP_NONE;
        kos_cap_t waiter = KOS_CAP_NONE;
        kos_task_t drv_task = KOS_TASK_NONE;
        kos_cap_t keep = KOS_CAP_NONE;
        kos::thread::Handle keeper;
        kos::thread::Handle writer;
        kos::thread::Handle drv;
        ArmHold hold;
        TAP_HOLD(hold.cap(&bare) and hold.cap(&waiter) and hold.task(&drv_task)
                 and hold.cap(&keep) and hold.thread(&keeper) and hold.thread(&writer)
                 and hold.thread(&drv));
        TAP_CHECK(kos_endpoint_create(&bare) == 0);
        TAP_CHECK(kos_cap_narrow(bare, KOS_CAP_SIGNAL | KOS_CAP_TRANSFER) == 0);
        int const refused = kos_console_publish(bare, KOS_TASK_NONE);
        int32_t const unseated = kos_send_timed(KOS_CAP_STDOUT, &probe, 0, CON_PARK_US);
        TAP_CHECK(hold.close(&bare) == 0);
        TAP_CHECK(refused == -KOS_EACCES);
        TAP_CHECK(unseated == -KOS_EBADF);

        // WAIT without HANDOUT is refused too.
        TAP_CHECK(kos_endpoint_create(&waiter) == 0);
        TAP_CHECK(kos_cap_narrow(waiter, KOS_CAP_WAIT) == 0);
        int const wait_refused = kos_console_publish(waiter, KOS_TASK_NONE);
        int32_t const wait_unseated = kos_send_timed(KOS_CAP_STDOUT, &probe, 0, CON_PARK_US);
        TAP_CHECK(hold.close(&waiter) == 0);
        TAP_CHECK(wait_refused == -KOS_EACCES);
        TAP_CHECK(wait_unseated == -KOS_EBADF);

        TAP_CHECK(kos_task_create(nullptr, 0, 0, &drv_task) == 0);
        TAP_CHECK(kos_sem_create(0, &keep) == 0);
        kos_cap_grant const keep_caps[] = {{keep, CH_FULL}};
        keeper = kos::thread::create_caps(con_keeper, nullptr, "conkeep", 10, keep_caps, 1,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                          nullptr, drv_task);
        TAP_CHECK(keeper.valid());

        // Narrowed at once, as the init keeps a served endpoint: vacated, HANDOUT alone. Not
        // held: console_publish_narrow closes it.
        TAP_CHECK(kos_endpoint_create(&g_con_ep) == 0);
        tap::census_expect(cap_census, -1);
        TAP_CHECK(kos_cap_narrow(g_con_ep, CON_KEPT) == 0);
        int const pub = kos_console_publish(g_con_ep, drv_task);
        // main holds WAIT again and the endpoint receives, so the send parks to its deadline.
        int32_t const parked = kos_send_timed(KOS_CAP_STDOUT, &probe, 0, CON_PARK_US);
        int const narrowed = kos_cap_narrow(g_con_ep, CON_KEPT);

        // Nobody receives, and the task lives.
        con_nonblock();
        ConNonblock const nb = g_nb;
        g_con_sent = -99;
        writer = irq_spawn(con_writer, nullptr, "conwrite", TAP_PRIO_PARKS, nullptr, 0);
        TAP_CHECK(writer.valid());
        kos_yield();
        int const blocked = writer.join(CON_PARK_US);
        // A receiver comes back and takes the parked line.
        kos_cap_grant caps[] = {{g_con_ep, KOS_CAP_WAIT}};
        drv = kos::thread::create_caps(con_driver, nullptr, "condrv", 10, caps, 1,
                                       KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr,
                                       drv_task);
        TAP_CHECK(drv.valid());
        TAP_CHECK(thread_end(&drv) == 0);
        TAP_CHECK(thread_end(&writer) == 0);
        // The receiver is gone again and the task lives, so the console is still the driver's.
        int32_t const still = kos_send_timed(KOS_CAP_STDOUT, &probe, 0, 0);
        // The task ends with its entry: the console comes back.
        (void)kos_sem_post(keep);
        TAP_CHECK(thread_end(&keeper) == 0);
        // main's HANDOUT is left.
        int32_t const after = kos_send_timed(KOS_CAP_STDOUT, &probe, 0, STALL_TOLERANT_US);
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
        TAP_CHECK(g_con_sent == 2);
        TAP_CHECK(still == -KOS_ETIMEDOUT);
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
        ArmHold hold;
        TAP_HOLD(hold.task(&drv_task));
        int const created = kos_task_create(nullptr, 0, 0, &drv_task);
        int const pub = kos_console_publish(g_con_ep, drv_task);
        int32_t const parked = kos_send_timed(KOS_CAP_STDOUT, &probe, 0, CON_PARK_US);
        int const narrowed = kos_cap_narrow(g_con_ep, CON_KEPT);
        int32_t const unserved = kos_send_timed(KOS_CAP_STDOUT, &probe, 0, 0);
        int const killed = kos_task_kill(drv_task);
        if (killed == 0)
        {
            drv_task = KOS_TASK_NONE;
        }
        int32_t const after = kos_send_timed(KOS_CAP_STDOUT, &probe, 0, STALL_TOLERANT_US);
        int const closed = kos_handle_close(g_con_ep);
        g_con_ep = KOS_CAP_NONE;
        tap::census_expect(cap_census, 1);
        int32_t const gone = kos_send_timed(KOS_CAP_STDOUT, &probe, 0, STALL_TOLERANT_US);
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

#if KICKOS_REBOOT
    // --- reboot-to-bootloader is privileged-only: the refusal arm only -------------
    // On picopi/pizero2350/teensy41 a main kos_reboot really reboots the board mid-run and
    // truncates the TAP stream.
    int g_reboot_rc = -99;
    void reboot_denied_worker(void*)
    {
        g_reboot_rc = kos_reboot();
    }
    void t_reboot_denied()
    {
        TAP_ASK(.workers = 1);
        g_reboot_rc = -99;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.thread(&w));
        w = kos::thread::create_caps(reboot_denied_worker, nullptr, "rbden", 10, nullptr, 0);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(g_reboot_rc == -KOS_EPERM);
    }
#endif // KICKOS_REBOOT (reboot refusal)

    // --- The authority capability: the non-privileged arm of the authority gates ------
    // Each authority gate is `privileged OR holds this AUTH_* bit`; main is unprivileged and
    // holds only the bits SELFTEST_AUTHORITY names, so the rest of the suite only exercises the
    // bit-held arm. The child is UNPRIVILEGED and holds AUTH_PINMUX and nothing else, so exactly
    // one gate must accept it and the rest must refuse. Acceptance reads as "not -KOS_EPERM": a
    // gate that lets the call through returns its own answer (-KOS_ENOSYS on a
    // declining-fallback target like the sim, -KOS_EINVAL where a chip owns the block).
    void auth_noop(void*) {}
    Atomic<int32_t, Order::RELAXED> g_auth_pinmux{-99};    // AUTH_PINMUX held    -> anything but -KOS_EPERM
    Atomic<int32_t, Order::RELAXED> g_auth_badpin{-99};    // and an out-of-range pin is refused
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
        // The bit it HOLDS: past the gate, so pinmux answers for itself, refusing an
        // out-of-range port and pin before any hardware write.
        g_auth_pinmux = kos_pinmux_set(99u, 0u, 0x10u);
        g_auth_badpin = kos_pinmux_set(0u, 99u, 0x10u);
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
        // An object cap narrows too: the sem cap at CH_DONE keeps SIGNAL and gives up the rest.
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
    }
    void t_authority_cap()
    {
        // main holds exactly its composition's word: the whole of it seats on a child, and
        // the one bit outside it does not.
        TAP_CHECK(g_self->authority == SELFTEST_AUTHORITY);
        TAP_ASK(.workers = 1);
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.thread(&w));
        w = kos::thread::create_caps(auth_noop, nullptr, "authA", 10, nullptr, 0,
                                     KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                     nullptr, 0, SELFTEST_AUTHORITY);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        w = kos::thread::create_caps(auth_noop, nullptr, "authP", 10, nullptr, 0,
                                     KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                     nullptr, 0, KOS_AUTH_PSTATE);
        TAP_CHECK(w.error() == -KOS_EPERM);
        kos_cap_grant caps[] = {{g_done, CH_FULL}};
        w = kos::thread::create_caps(auth_worker, nullptr, "authW", 10, caps, 1,
                                     KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                     nullptr, 0, /*authority=*/KOS_AUTH_PINMUX);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        int32_t const auth_pinmux = g_auth_pinmux;
        TAP_CHECK(auth_pinmux != -KOS_EPERM and auth_pinmux < 0);
        int32_t const auth_badpin = g_auth_badpin;
        TAP_CHECK(auth_badpin != -KOS_EPERM and auth_badpin < 0);
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
    constexpr RamAsk TA_RAM = {TA_BLOCK};
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
            (void)kos_thread_join(h, STALL_TOLERANT_US);
        }
        return rc;
    }
    void task_auth_worker(void* arg)
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
    }
    void t_task_authority()
    {
        TAP_ASK(.workers = 2, .tasks = 1);
        g_ta_block = st_ram<TA_RAM, 0>();
        TAP_CHECK(g_ta_block != nullptr);
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.thread(&w));
        for (uintptr_t run = 0; run < 2; ++run)
        {
            g_ta[run] = {-99, -99, -99};
            uint32_t auth = KOS_AUTH_MEMORY;
            if (run == 1)
            {
                auth |= KOS_AUTH_TASKS;
            }
            w = kos::thread::create_caps(task_auth_worker, reinterpret_cast<void*>(run), "taW",
                                         10, nullptr, 0, KOS_POLICY_FIFO, 0,
                                         /*privileged=*/false, nullptr, 0, auth);
            TAP_CHECK(w.valid());
            TAP_CHECK(hold.joined());
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
    constexpr RamAsk PE_RAM = {1};
    void periph_enable_worker(void*)
    {
        g_pe_unheld = kos_periph_enable(PE_BASE);
        void* const p = st_ram<PE_RAM, 0>();
        if (p != nullptr and kos_mem_self_grant(p, 1, 0) == 0)
        {
            g_pe_ram = kos_periph_enable(reinterpret_cast<uintptr_t>(p));
            g_pe_ram_ran = 1;
        }
    }
    void t_periph_enable_unheld()
    {
        TAP_ASK(.workers = 1);
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.thread(&w));
        w = kos::thread::create_caps(periph_enable_worker, nullptr, "peW", 10, nullptr, 0,
                                     KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                     nullptr, 0, /*authority=*/KOS_AUTH_MEMORY);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        int32_t const pe_unheld = g_pe_unheld;
        TAP_CHECK(pe_unheld == -KOS_EPERM);
        int const pe_ram_ran = g_pe_ram_ran;
        TAP_CHECK(pe_ram_ran == 1);
        int32_t const pe_ram = g_pe_ram;
        TAP_CHECK(pe_ram == -KOS_EPERM);
    }

    // --- Privileged register write: the same possession gate, plus the refusal ------
    // Arm 2 finds its DEV window by TRYING the spawn; a board that mints no window reports
    // PARTIAL. Arm 3 runs from the UNHELD worker, alignment and wrap being checked before
    // possession. Arm 4 reuses arm 2's window. PRW_OFFSET MUST be 4-aligned or arm 2 stops
    // short of the chip layer, and the BASE keeps it unnameable: the discovery loop steps
    // 0x1000 and the tree's only allowlist is XMC4800's.
    constexpr uintptr_t PRW_OFFSET = 0x4u;
    // Pow2 >= the 32 B PMSA minimum; the discovery step is a multiple of it, so every
    // candidate base stays WIN-aligned (PMSA masks an unaligned base down).
    constexpr uint32_t PRW_WIN = 0x100u;
    // One byte of .bss carries every arm's verdict, not a result word per arm: .bss comes out
    // of the arena on the smallest parts. Each worker stores only its own bits, and the bits
    // main reads after a join are that worker's alone.
    constexpr uint8_t PRW_UNHELD_OK = 1u << 0;     // arm 1: unheld base refused
    constexpr uint8_t PRW_HELD_RAN = 1u << 1;      // arm 2: a window was minted
    constexpr uint8_t PRW_HELD_OK = 1u << 2;       // arm 2: declined by the chip layer
    constexpr uint8_t PRW_MISALIGNED_OK = 1u << 3; // arm 3: offset not 4-aligned
    constexpr uint8_t PRW_WRAP_OK = 1u << 4;       // arm 3: base + offset wraps
    constexpr uint8_t PRW_PAST_END_OK = 1u << 5;   // arm 4: first word beyond the window
    constexpr uint8_t PRW_FAR_OK = 1u << 6;        // arm 4: 4 KiB beyond the window
    Atomic<uint8_t, Order::RELAXED> g_prw{0};
    void periph_reg_write_held_worker(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);
        uint32_t seen = PRW_HELD_RAN;
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
        g_prw = static_cast<uint8_t>(seen);
    }
    void periph_reg_write_worker(void*)
    {
        uint32_t seen = 0;
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
        g_prw = static_cast<uint8_t>(seen);
    }
    void prw_bare_worker(void*)
    {
    }
    void t_periph_reg_write_unheld()
    {
        TAP_ASK(.workers = 1);
        kos::thread::Handle w;
        kos::thread::Handle holder;
        ArmHold hold;
        TAP_HOLD(hold.thread(&w) and hold.thread(&holder));
        w = kos::thread::create_caps(periph_reg_write_worker, nullptr, "prwW", 10, nullptr, 0,
                                     KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                     nullptr, 0, /*authority=*/KOS_AUTH_MEMORY);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        w = kos::thread::Handle();
        uint8_t const prw = g_prw;
        TAP_CHECK((prw & PRW_UNHELD_OK) != 0);
        // Arm 3: no DEV window needed, so these run on EVERY board including the ones
        // that mint none.
        TAP_CHECK((prw & PRW_MISALIGNED_OK) != 0);
        TAP_CHECK((prw & PRW_WRAP_OK) != 0);

        // Arms 2 and 4 share one window.
        for (uintptr_t b = 0x40000000u; b < 0x40100000u; b += 0x1000u)
        {
            holder = kos::thread::create(periph_reg_write_held_worker,
                                         reinterpret_cast<void*>(b), "prwH", 10,
                                         KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                         nullptr, 0, nullptr, 0,
                                         DeviceWindow(b, PRW_WIN).list(), 1,
                                         nullptr, 0, KOS_AUTH_MEMORY);
            if (holder.valid() or holder.error() == -KOS_ENOMEM)
            {
                break;
            }
        }
        // A window the space cannot map also answers -KOS_ENOMEM: the same spawn without it
        // tells the two apart.
        if (holder.error() == -KOS_ENOMEM)
        {
            holder = kos::thread::create(prw_bare_worker, nullptr, "prwB", 10);
            TAP_CHECK(holder.valid());
            TAP_CHECK(hold.joined());
            holder = kos::thread::Handle();
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
        TAP_CHECK(hold.joined());
        uint8_t const prw_held = g_prw;
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
    void pvs_worker(void* arg)
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
    }
    void t_periph_reg_write_mask()
    {
        TAP_ASK(.workers = 1);
        // Try every candidate, in the sim's own order: exactly one is mapped, and which
        // one depends on the host's address space, not on this test.
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.thread(&w));
        for (uintptr_t b : PVS_BASES)
        {
            w = kos::thread::create(pvs_worker, reinterpret_cast<void*>(b), "pvsW", 10,
                                    KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0,
                                    nullptr, 0, DeviceWindow(b, PVS_WIN).list(), 1,
                                    nullptr, 0, KOS_AUTH_MEMORY);
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
        TAP_CHECK(hold.joined());
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
        kos_sem_wait(CH_DEVHOLD, KOS_TIMEOUT_NONE);
    }
    // The respawn from a death wake: the server holding the window takes the call and exits
    // holding it, and its teardown wakes the caller. The window must already be free for the
    // caller's respawn.
    Atomic<int32_t, Order::RELAXED> g_wl_call{-99};
    Atomic<int32_t, Order::RELAXED> g_wl_respawn{-99};
    void wl_noop(void*) {}
    void wl_server(void*) // caps: done@1, E(WAIT)@2
    {
        char b[4];
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, 2, 0, KOS_TIMEOUT_NONE);
        (void)kos_reply_recv(KOS_CAP_NONE, b, kos_call_lens_pack(0, sizeof(b)), &o);
        kos_exit(0); // holding the call, whose caller the teardown answers -KOS_EPIPE
    }
    void wl_caller(void* arg) // caps: done@1, E(SIGNAL)@2
    {
        uintptr_t const pv = reinterpret_cast<uintptr_t>(arg);
        char b[4] = {};
        g_wl_call = kos_call_timed(2, b, sizeof(b), sizeof(b), STALL_TOLERANT_US);
        auto const again = kos::thread::create(wl_noop, nullptr, "wlN", 10, KOS_POLICY_FIFO, 0,
                                               false, nullptr, 0, nullptr, 0,
                                               DeviceWindow(pv, PVS_WIN).list(), 1);
        g_wl_respawn = again.error();
        if (again.valid())
        {
            (void)again.join(STALL_TOLERANT_US);
        }
    }
    void t_window_list()
    {
        kos_cap_t hold_sem = KOS_CAP_NONE;
        kos_cap_t ep = KOS_CAP_NONE;
        kos::thread::Handle twice;
        kos::thread::Handle h;
        kos::thread::Handle second;
        kos::thread::Handle sv;
        kos::thread::Handle cl;
        ArmHold hold;
        TAP_HOLD(hold.cap(&hold_sem) and hold.cap(&ep) and hold.thread(&twice) and hold.thread(&h)
                 and hold.thread(&second) and hold.thread(&sv) and hold.thread(&cl));
        TAP_ASK(.workers = 2, .sems = 1, .endpoints = 1);
        TAP_CHECK(kos_sem_create(0, &hold_sem) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {hold_sem, CH_FULL}};
        g_wl = 0;
        uintptr_t pv = 0;
        for (uintptr_t b : PVS_BASES)
        {
            kos_window const dup[] = {DeviceWindow(b, PVS_WIN).w, DeviceWindow(b, PVS_WIN).w};
            twice = kos::thread::create(wl_holder, nullptr, "wltwice", 10, KOS_POLICY_FIFO, 0,
                                        false, nullptr, 0, nullptr, 0, dup, 2, caps, 2,
                                        KOS_AUTH_MEMORY);
            if (twice.error() != -KOS_EINVAL)
            {
                pv = b; // the mapped candidate: every other one is unencodable
                break;
            }
        }
        TAP_CHECK(twice.error() == -KOS_EBUSY);
        kos_window const both[] = {DeviceWindow(pv, PVS_WIN).w,
                                   DeviceWindow(pv + PVS_WIN, PVS_WIN).w};
        h = kos::thread::create(wl_holder, reinterpret_cast<void*>(pv), "wlboth", 10,
                                KOS_POLICY_FIFO, 0, false, nullptr, 0, nullptr, 0, both, 2, caps, 2,
                                KOS_AUTH_MEMORY);
        TAP_CHECK(h.valid());
        second = kos::thread::create(wl_holder, nullptr, "wlsecond", 10, KOS_POLICY_FIFO, 0, false,
                                     nullptr, 0, nullptr, 0,
                                     DeviceWindow(pv + PVS_WIN, PVS_WIN).list(), 1, caps, 2,
                                     KOS_AUTH_MEMORY);
        TAP_CHECK(second.error() == -KOS_EBUSY);
        TAP_CHECK(kos_sem_post(hold_sem) == 0);
        // Its windows are free once it is gone, which the respawn below needs.
        TAP_CHECK(thread_end(&h) == 0);
        unsigned char const wl = g_wl;
        TAP_CHECK((wl & WL_FIRST_OK) != 0);
        TAP_CHECK((wl & WL_SECOND_OK) != 0);
        TAP_CHECK((wl & WL_ADDR_OK) != 0);

        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        g_wl_call = -99;
        g_wl_respawn = -99;
        kos_cap_grant scaps[] = {{g_done, CH_FULL}, {ep, EP_WAIT_ONLY}};
        kos_cap_grant ccaps[] = {{g_done, CH_FULL}, {ep, EP_SIGNAL_ONLY}};
        sv = kos::thread::create(wl_server, nullptr, "wlS", 10, KOS_POLICY_FIFO, 0, false, nullptr,
                                 0, nullptr, 0, DeviceWindow(pv, PVS_WIN).list(), 1, scaps, 2,
                                 KOS_AUTH_MEMORY);
        cl = kos::thread::create(wl_caller, reinterpret_cast<void*>(pv), "wlC", 12,
                                 KOS_POLICY_FIFO, 0, false, nullptr, 0, nullptr, 0, nullptr, 0,
                                 ccaps, 2, KOS_AUTH_MEMORY);
        TAP_CHECK(sv.valid() and cl.valid());
        TAP_CHECK(hold.joined());
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

    Atomic<int, Order::RELAXED> g_join_ran{0};
    void join_target(void*)
    {
        g_join_ran = 1;
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
    }

    void t_thread_join()
    {
        TAP_CHECK(g_main != KOS_THREAD_NONE);
        TAP_ASK(.workers = 1);
        log_reset();
        g_join_ran = 0;
        kos::thread::Handle w;
        kos::thread::Handle s;
        ArmHold hold;
        TAP_HOLD(hold.thread(&w) and hold.thread(&s));
        w = irq_spawn(join_target, nullptr, "join", TAP_PRIO_AFTER, nullptr, 0);
        TAP_CHECK(w.valid());
        // The parked path: the target has not run when the join is entered.
        int const parked_rc = w.join(STALL_TOLERANT_US);
        int const ran = g_join_ran;
        TAP_CHECK(parked_rc == 0);
        TAP_CHECK(ran == 1);
        // The generation bumps at RECLAIM and not at exit, and main has spawned nothing
        // since, so this handle still names the slot its EXITED occupant holds. The bound
        // is what makes the case total instead of a hang: a kernel that read that state as
        // "still running" answers -KOS_ETIMEDOUT here, and one that read it as a stale
        // handle answers -KOS_EBADF.
        int const exited_rc = kos_thread_join(w.id(), JOIN_GENEROUS_US);
        TAP_CHECK(exited_rc == 0);
        w = kos::thread::Handle();
        // Refusals that need no target thread: a handle the pool never seats, one whose
        // index is past every slot, and main naming itself.
        TAP_CHECK(kos_thread_join(KOS_THREAD_NONE, JOIN_GENEROUS_US) == -KOS_EBADF);
        TAP_CHECK(kos_thread_join(0x7fffffffu, JOIN_GENEROUS_US) == -KOS_EBADF);
        TAP_CHECK(kos_thread_join(g_main, JOIN_GENEROUS_US) == -KOS_EDEADLK);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        s = kos::thread::create_caps(join_stranger, nullptr, "jstr", 10, caps, 2);
        TAP_CHECK(s.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(log_eq("P")); // parenthood is the whole gate, and nothing delegates it
    }

    // --- Join: a handle whose slot changed hands --------------------------------
    // ThreadPool::resolve has two ways to answer nullptr, and t_thread_join's refusals reach only
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
        kos_sem_wait(1, KOS_TIMEOUT_NONE);
    }

    void t_join_stale_gen()
    {
        TAP_ASK(.workers = JOIN_RESEAT_TRIES, .sems = 1);
        kos_cap_t gate = KOS_CAP_NONE;
        kos::thread::Handle first;
        kos::thread::Handle probes[JOIN_RESEAT_TRIES];
        ArmHold hold;
        TAP_HOLD(hold.cap(&gate) and hold.thread(&first));
        for (int i = 0; i < JOIN_RESEAT_TRIES; i++)
        {
            TAP_HOLD(hold.thread(&probes[i]));
        }
        first = kos::thread::create(join_probe, nullptr, "jgn1", 10);
        TAP_CHECK(first.valid());
        kos_thread_t const stale = first.id();
        TAP_CHECK(thread_end(&first) == 0); // EXITED, and its slot is now reclaimable
        TAP_CHECK(kos_sem_create(0, &gate) == 0);
        kos_cap_grant hcaps[] = {{gate, CH_FULL}};
        int made = 0;
        int reseated = -1;
        for (int i = 0; i < JOIN_RESEAT_TRIES; i++)
        {
            probes[i] = kos::thread::create_caps(join_holder, nullptr, "jgn2", 10, hcaps, 1);
            TAP_CHECK(probes[i].valid());
            made++;
            if ((probes[i].id() & JOIN_SLOT_MASK) == (stale & JOIN_SLOT_MASK))
            {
                reseated = i;
                break;
            }
        }
        // The claim, made only where the premise is a fact: the reseating spawn bumped the
        // generation, so the old handle carries an index the pool DOES seat and a generation
        // nothing holds, which is ThreadPool::resolve's second nullptr branch and no other.
        int stale_rc = 0;
        kos_thread_t reseated_id = KOS_THREAD_NONE;
        if (reseated >= 0)
        {
            reseated_id = probes[reseated].id();
            stale_rc = kos_thread_join(stale, JOIN_GENEROUS_US);
        }
        for (int i = 0; i < made; i++)
        {
            TAP_CHECK(kos_sem_post(gate) == 0);
        }
        TAP_CHECK(hold.joined());
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

    void join_slow(void*) // caps: gate@1
    {
        kos_sem_wait(1, KOS_TIMEOUT_NONE);
    }

    void t_join_timeout()
    {
        TAP_ASK(.workers = 1, .sems = 1);
        kos_cap_t gate = KOS_CAP_NONE;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.cap(&gate) and hold.thread(&w));
        TAP_CHECK(kos_sem_create(0, &gate) == 0);
        kos_cap_grant caps[] = {{gate, KOS_CAP_WAIT}};
        w = irq_spawn(join_slow, nullptr, "jslo", TAP_PRIO_AFTER, caps, 1);
        TAP_CHECK(w.valid());
        uint64_t const t0 = kos_clock_now();
        int const rc = w.join(JOIN_TIMEOUT_US);
        uint32_t const waited_us = static_cast<uint32_t>((kos_clock_now() - t0) / 1000ull);
        TAP_CHECK(rc == -KOS_ETIMEDOUT); // the target is still running, and nothing waits on it
        // Both clock reads bracket the syscall, so this cannot pass on a deadline the
        // kernel fired immediately.
        TAP_CHECK(waited_us >= JOIN_TIMEOUT_US);
        TAP_CHECK(kos_sem_post(gate) == 0);
        // The expiry cleared the wait edge, so the target's exit sweep finds nothing to
        // wake and this is a FRESH park. It must still be woken by that same exit.
        TAP_CHECK(thread_end(&w) == 0);
    }

    // --- Tasks: the handle codec, the creator gate, and the group kill ----------
    //
    // A task handle carries a generation over a BIASED index, so the all-zero word is
    // KOS_TASK_NONE and no live task is ever named by it. The gate is CREATORSHIP and takes
    // a second thread to witness, so this arm covers what one thread can see: every refusal
    // the codec produces, and that a hold, once dropped, names nothing.
    void t_task_handles()
    {
        TAP_ASK(.workers = 1, .tasks = 1);
        kos_task_t task = KOS_TASK_NONE;
        kos::thread::Handle probe;
        ArmHold hold;
        TAP_HOLD(hold.task(&task) and hold.thread(&probe));
        // The out-pointer is validated BEFORE the group exists: a null one is malformed and a
        // misaligned one would take a privileged store the kernel must not make. Checked first,
        // because a mint that cannot deliver its handle leaves a task nothing can name.
        TAP_CHECK(kos_task_create(nullptr, 0, 0, nullptr) == -KOS_EINVAL);
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
        probe = kos::thread::create(join_probe, nullptr, "trsv", 10);
        TAP_CHECK(probe.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(kos_task_kill(task) == 0);
        kos_task_t const killed = task;
        task = KOS_TASK_NONE;
        // The kill DROPPED the hold, and an empty group with no hold is a free slot, so the
        // handle now names nothing. Killing twice is not idempotent, and must not be.
        TAP_CHECK(kos_task_kill(killed) == -KOS_EBADF);
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
        (void)kos_send_timed(KOS_SPAWN_DELEGATED_CAP0, answer, sizeof(answer),
                             STALL_TOLERANT_US);
        kos_exit(0);
    }

    void t_task_creator_gate()
    {
        TAP_ASK(.workers = 1, .tasks = 1, .endpoints = 1);
        kos_cap_t ep = KOS_CAP_NONE;
        kos_task_t task = KOS_TASK_NONE;
        kos::thread::Handle stranger;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.task(&task) and hold.thread(&stranger));
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &task) == 0);
        kos_cap_grant const caps[1] = {{ep, CH_FULL}};
        stranger = kos::thread::create(
            task_stranger, reinterpret_cast<void*>(static_cast<uintptr_t>(task)), "tstr", 10,
            KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0, nullptr, 0, nullptr, 0,
            caps, 1);
        TAP_CHECK(stranger.valid());
        unsigned char answer[3] = {0u, 0u, 0u};
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, ep, 0, STALL_TOLERANT_US);
        TAP_CHECK(kos_reply_recv(KOS_CAP_NONE, answer, kos_call_lens_pack(0, sizeof(answer)),
                                 &opts)
                  == 3);
        TAP_CHECK(answer[0] == 1u); // a stranger cannot seat a member
        TAP_CHECK(answer[1] == 1u); // nor end the group
        TAP_CHECK(answer[2] == 1u); // nor watch it
        TAP_CHECK(hold.joined());
        // Still ours, so still killable: the refusals above cost the group nothing.
        TAP_CHECK(kos_task_kill(task) == 0);
        task = KOS_TASK_NONE;
        TAP_CHECK(hold.close(&ep) == 0);
    }

    // --- The creator's reports: a first receive, and a death ------------------------------
    // A worker holding the task authority is the creator, so the reports go to whoever armed
    // and never to main by name.
    // It arms the watch with a badged copy of a notification it binds, naming one of
    // two endpoints, and closes the copy: the watch names the object, and the binding still
    // receives through it. The member waits on the OTHER endpoint first, which reports nothing,
    // then on the watched one, which reports ready; a second wait there reports nothing more;
    // its exit, the entry's, reports the end and then the death. Each "nothing" is read with no
    // wait once the creator's send was taken, the member then held on the other endpoint until
    // the creator's next send there. The state follows each step, and a released task answers
    // -KOS_EBADF.
    constexpr uint32_t TW_BIT = 5;
    constexpr uint8_t TW_CREATOR_PRIO = 14;
    constexpr uint8_t TW_MEMBER_PRIO = 11;
    void tw_recv(kos_cap_t ep)
    {
        char b[4];
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, ep, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
        (void)kos_reply_recv(KOS_CAP_NONE, b, kos_call_lens_pack(0, sizeof(b)), &o);
    }
    void tw_member(void*) // caps: watched E(WAIT)@1, other E(WAIT)@2
    {
        tw_recv(2);
        tw_recv(2);
        tw_recv(1);
        tw_recv(1);
        tw_recv(2);
        kos_exit(0);
    }
    struct TwRun
    {
        int setup, armed, closed, before, quiet, quiet_state, ready, ready_state, again, ended,
            ended_state, dead, dead_state, killed, stale;
        uint32_t ready_bits, ended_bits;
    };
    TwRun g_tw;
    void tw_play(kos_task_t t, kos_cap_t n, kos_cap_t ep, kos_cap_t other, TwRun* r,
                 uint64_t until)
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
                                                nullptr, 0, /*authority=*/0, nullptr, t,
                                                nullptr, 0, TAP_PIN_CORE);
        if (not m.valid())
        {
            return;
        }
        (void)kos_send_timed(other, "x", 1, left_us_until(until));
        r->quiet = kos_notify_wait(n, mask, 0u, &bits);
        r->quiet_state = kos_task_state(t);
        (void)kos_send_timed(other, "x", 1, left_us_until(until));
        r->ready = kos_notify_wait(n, mask, left_us_until(until), &r->ready_bits);
        r->ready_state = kos_task_state(t);
        (void)kos_send_timed(ep, "x", 1, left_us_until(until));
        (void)kos_send_timed(ep, "x", 1, left_us_until(until));
        r->again = kos_notify_wait(n, mask, 0u, &bits);
        (void)kos_send_timed(other, "x", 1, left_us_until(until));
        r->ended = kos_notify_wait(n, mask, left_us_until(until), &r->ended_bits);
        r->ended_state = kos_task_state(t);
        r->dead = 0;
        if (r->ended_state >= 0 and (r->ended_state & KOS_TASK_DEAD) == 0)
        {
            r->dead = kos_notify_wait(n, mask, left_us_until(until), &bits);
        }
        r->dead_state = kos_task_state(t);
        r->killed = kos_task_kill(t);
        r->stale = kos_task_state(t);
    }
    void tw_creator(void*)
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
            tw_play(t, n, ep, other, &g_tw, deadline_in(CREATOR_US));
            (void)kos_notify_unbind(n);
        }
        (void)kos_task_kill(t);
        (void)kos_handle_close(n);
        (void)kos_handle_close(ep);
        (void)kos_handle_close(other);
    }
    void t_task_watch_reports()
    {
        // The creator's task, notification and two endpoints come out of main's task budgets.
        TAP_ASK(.workers = 2, .tasks = 1, .endpoints = 2, .notifies = 1);
        g_tw = {-99, -99, -99, -99, -99, -99, -99, -99, -99, -99, -99, -99, -99, -99, -99, 0u,
                0u};
        kos::thread::Handle w;
        ArmHold hold(CREATOR_HOLD_US);
        TAP_HOLD(hold.thread(&w));
        w = kos::thread::create(tw_creator, nullptr, "twC", TW_CREATOR_PRIO, KOS_POLICY_FIFO, 0,
                                /*privileged=*/false, nullptr, 0, nullptr, 0, nullptr, 0,
                                nullptr, 0, KOS_AUTH_TASKS, nullptr, KOS_TASK_NONE,
                                TAP_PIN_CORE);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
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
    static_assert(CREATOR_US < TX_WAIT_US, "a creator must give up before its workers do");
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
    bool tx_await_until(kos_task_t t, int bits, uint64_t give_up)
    {
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
    bool tx_await(kos_task_t t, int bits)
    {
        return tx_await_until(t, bits, tx_give_up());
    }
    // Never enters the kernel once it runs, and counts its turns where a member of its own task
    // reads them.
    volatile uint32_t g_tx_turns = 0;
    void tx_spins(void*)
    {
        while (true)
        {
            g_tx_turns = g_tx_turns + 1u;
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
    void tx_stranger(void* arg)
    {
        uintptr_t const t = reinterpret_cast<uintptr_t>(arg);
        int status = 0;
        g_tx_stranger = kos_task_exit_status(static_cast<kos_task_t>(t), &status);
    }
    // Tells main on `ep` whether the spinner had spun.
    constexpr uint64_t TX_SPIN_NS = 5000000ull;
    void tx_report_spun(kos_cap_t ep)
    {
        kos_sleep_ns(TX_SPIN_NS);
        char spun = 'n';
        if (g_tx_turns != 0u)
        {
            spun = 's';
        }
        (void)kos_send_timed(ep, &spun, 1, TX_WAIT_US);
    }
    void tx_entry_returns(void*) // caps: E(SIGNAL)@1
    {
        (void)kos::thread::create(tx_spins, nullptr, "txs", KICKOS_PRIO_MIN,
                                  KOS_POLICY_FIFO, 0, false, nullptr, 0, nullptr, 0, nullptr, 0,
                                  nullptr, 0, 0, nullptr, KOS_TASK_NONE, TAP_PIN_CORE);
        tx_report_spun(1);
    }
    void tx_entry_exits(void*) // caps: go@1, E(SIGNAL)@2
    {
        auto const quits = kos::thread::create(tx_quits, nullptr, "txq", 9);
        if (quits.valid())
        {
            (void)quits.join(TX_WAIT_US);
        }
        (void)kos_send_timed(2, "r", 1, TX_WAIT_US);
        kos_sem_wait(1, KOS_TIMEOUT_NONE);
        kos_exit(TX_ENTRY_EXIT);
    }
    // A timed receive of one byte on `ep`: what kos_reply_recv answered.
    int32_t tx_recv_one(kos_cap_t ep)
    {
        char r = 0;
        struct kos_reply_recv_opts o;
        kos_reply_recv_opts_init(&o, ep, KOS_RECV_NO_INFO, TX_WAIT_US);
        return kos_reply_recv(KOS_CAP_NONE, &r, kos_call_lens_pack(0, 1), &o);
    }
    void t_task_exit_entry_return()
    {
        TAP_ASK(.workers = 2, .tasks = 1, .endpoints = 1, .notifies = 1);
        kos_cap_t ready = KOS_CAP_NONE;
        kos_cap_t n = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        kos::thread::Handle stranger;
        kos::thread::Handle entry;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ready) and hold.bound(&n) and hold.task(&t)
                 and hold.thread(&stranger) and hold.thread(&entry));
        TAP_CHECK(kos_endpoint_create(&ready) == 0);
        TAP_CHECK(kos_notify_create(&n) == 0);
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &t) == 0);
        constexpr uint32_t mask = 1u << TW_BIT;
        kos_cap_t badged = KOS_CAP_NONE;
        int const bound = kos_notify_bind(n);
        int armed = kos_notify_badge(n, TW_BIT, &badged);
        if (armed == 0)
        {
            armed = kos_task_watch(t, badged, KOS_CAP_NONE);
        }
        (void)kos_handle_close(badged);
        g_tx_stranger = -99;
        stranger = kos::thread::create(tx_stranger,
                                       reinterpret_cast<void*>(static_cast<uintptr_t>(t)), "txx",
                                       10);
        TAP_CHECK(stranger.valid());
        TAP_CHECK(thread_end(&stranger, TX_WAIT_US) == 0);
        kos_cap_grant const caps[] = {{ready, KOS_CAP_SIGNAL}};
        g_tx_turns = 0;
        entry = kos::thread::create(tx_entry_returns, nullptr, "txe", 10, KOS_POLICY_FIFO, 0,
                                    false, nullptr, 0, nullptr, 0, nullptr, 0, caps, 1, 0, nullptr,
                                    t, TAP_PIN_CORE);
        TAP_CHECK(entry.valid());
        int status = -99;
        int const live = kos_task_exit_status(t, &status);
        char spun = 0;
        bool const heard = report_await(ready, &spun, 1);
        uint32_t bits = 0;
        int const ended = kos_notify_wait(n, mask, TX_WAIT_US, &bits);
        int const state = kos_task_state(t);
        bool const dead = tx_await(t, KOS_TASK_DEAD);
        int const later = kos_task_state(t);
        int const got = kos_task_exit_status(t, &status);
        int const joined = thread_end(&entry, TX_WAIT_US);
        int const refused = tx_seat(t);
        kos_task_t const stale = t;
        int const slain = task_end(&t, TX_WAIT_US);
        TAP_CHECK(bound == 0 and armed == 0);
        TAP_CHECK(live == -KOS_EBUSY and g_tx_stranger == -KOS_EPERM);
        TAP_CHECK(heard);
        if (spun != 's')
        {
            TAP_SKIP_VACUOUS("the sibling never ran its own code before the entry returned");
            return;
        }
        TAP_CHECK(ended == 0 and bits == mask and state >= 0 and (state & KOS_TASK_ENDED) != 0);
        TAP_CHECK(dead and later == (KOS_TASK_DEAD | KOS_TASK_ENDED));
        TAP_CHECK(got == 0 and status == 0 and joined == 0);
        TAP_CHECK(refused == -KOS_EBUSY);
        TAP_CHECK(slain == 0);
        int stale_status = -99;
        TAP_CHECK(kos_task_exit_status(stale, &stale_status) == -KOS_EBADF
                  and stale_status == -99);
    }
    void t_task_exit_member_exit()
    {
        TAP_ASK(.workers = 2, .tasks = 1, .sems = 1, .endpoints = 1);
        kos_cap_t go = KOS_CAP_NONE;
        kos_cap_t ready = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        kos::thread::Handle entry;
        ArmHold hold;
        TAP_HOLD(hold.cap(&go) and hold.cap(&ready) and hold.task(&t) and hold.thread(&entry));
        TAP_CHECK(kos_sem_create(0, &go) == 0);
        TAP_CHECK(kos_endpoint_create(&ready) == 0);
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &t) == 0);
        kos_cap_grant const caps[] = {{go, KOS_CAP_WAIT}, {ready, KOS_CAP_SIGNAL}};
        entry = kos::thread::create(tx_entry_exits, nullptr, "txe", 10, KOS_POLICY_FIFO, 0, false,
                                    nullptr, 0, nullptr, 0, nullptr, 0, caps, 2, 0, nullptr, t);
        TAP_CHECK(entry.valid());
        int32_t const said = tx_recv_one(ready);
        // The sibling has exited with its own code, and the task lives on.
        int const state = kos_task_state(t);
        int status = -99;
        int const live = kos_task_exit_status(t, &status);
        TAP_CHECK(kos_sem_post(go) == 0);
        bool const dead = tx_await(t, KOS_TASK_DEAD);
        int const final_state = kos_task_state(t);
        int const got = kos_task_exit_status(t, &status);
        int const joined = thread_end(&entry, TX_WAIT_US);
        int const refused = tx_seat(t);
        int const killed = kos_task_kill(t);
        if (killed == 0)
        {
            t = KOS_TASK_NONE;
        }
        TAP_CHECK(said == 1);
        TAP_CHECK(state == KOS_TASK_LIVE and live == -KOS_EBUSY);
        TAP_CHECK(dead and final_state == (KOS_TASK_DEAD | KOS_TASK_ENDED));
        TAP_CHECK(got == 0 and status == TX_ENTRY_EXIT);
        TAP_CHECK(refused == -KOS_EBUSY);
        TAP_CHECK(joined == 0 and killed == 0);
    }

    // --- A task is dead only once its members' teardown is done --------------------------
    // A creator calls the endpoint the entry alone receives on. The entry fills its table and
    // returns; its teardown closes that receive right first, which ends the call, and sweeps the
    // rest across lock gaps. The creator reads the task then: ended, no member, not yet dead.
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
    void td_creator(void*)
    {
        uint64_t const until = deadline_in(CREATOR_US);
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
                g_td.called = kos_call_timed(ep, b, sizeof(b), sizeof(b), left_us_until(until));
                g_td.mid = kos_task_state(t);
                g_td.dead = tx_await_until(t, KOS_TASK_DEAD, until);
                g_td.final_state = kos_task_state(t);
            }
            if (entry.valid())
            {
                (void)entry.join(left_us_until(until));
            }
        }
        (void)kos_task_kill(t);
        (void)kos_handle_close(ep);
    }
    void t_task_dead_after_sweep()
    {
        // The creator's task and endpoint come out of main's task budgets.
        TAP_ASK(.workers = 2, .tasks = 1, .endpoints = 1);
        g_td = {-99, -99, -99, 0, -99};
        kos::thread::Handle w;
        ArmHold hold(CREATOR_HOLD_US);
        TAP_HOLD(hold.thread(&w));
        w = kos::thread::create(td_creator, nullptr, "tdc", 12, KOS_POLICY_FIFO, 0, false, nullptr,
                                0, nullptr, 0, nullptr, 0, nullptr, 0, KOS_AUTH_TASKS, nullptr,
                                KOS_TASK_NONE, 1u);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        TdRun const r = g_td;
        TAP_CHECK(r.setup == 0);
        TAP_CHECK(r.called < 0);
        TAP_CHECK(r.mid == KOS_TASK_ENDED);
        TAP_CHECK(r.dead == 1 and r.final_state == (KOS_TASK_DEAD | KOS_TASK_ENDED));
    }

    // --- A task is dead only once EVERY member's teardown is done -------------------------
    // Two members with uneven loads: the sibling holds a wide table whose last slot is the only
    // receive right of an endpoint a watcher calls, the entry a single capability. The sibling
    // exits first; its sweep closes the endpoint the entry calls. The watcher's call ends only
    // when the sibling's sweep reaches its last slot: the watcher's stamp has to come before the
    // creator's death stamp.
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
    void te_creator(void*)
    {
        uint64_t const until = deadline_in(CREATOR_US);
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
            uint64_t const give_up = until;
            while (g_te.setup == 0 and kos_clock_now() < give_up)
            {
                uint32_t bits = 0;
                (void)kos_notify_wait(n, 1u << TW_BIT, left_us_until(until), &bits);
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
                (void)watcher.join(left_us_until(until));
            }
            if (entry.valid())
            {
                (void)entry.join(left_us_until(until));
            }
            if (sibling.valid())
            {
                (void)sibling.join(left_us_until(until));
            }
            (void)kos_notify_unbind(n);
        }
        (void)kos_handle_close(badged);
        (void)kos_handle_close(n);
        (void)kos_task_kill(t);
        (void)kos_handle_close(probe);
        (void)kos_handle_close(ep);
    }
    void t_task_dead_after_every_sweep()
    {
        // The creator's task, endpoints and notification come out of main's task budgets.
        TAP_ASK(.workers = 4, .tasks = 1, .endpoints = 2, .notifies = 1);
        g_te = {-99, -99, -99, 0, 0, -99, -99, -99, -99, -99, -99};
        g_te_seq = 0;
        kos::thread::Handle w;
        ArmHold hold(CREATOR_HOLD_US);
        TAP_HOLD(hold.thread(&w));
        w = kos::thread::create(te_creator, nullptr, "tec", 14, KOS_POLICY_FIFO, 0, false, nullptr,
                                0, nullptr, 0, nullptr, 0, nullptr, 0, KOS_AUTH_TASKS, nullptr,
                                KOS_TASK_NONE, 1u);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
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
        TAP_ASK(.workers = 1, .tasks = 1);
        kos_task_t t = KOS_TASK_NONE;
        kos::thread::Handle entry;
        ArmHold hold;
        TAP_HOLD(hold.task(&t) and hold.thread(&entry));
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &t) == 0);
        entry = kos::thread::create(tx_driver_traps, nullptr, "txtrap", 10, KOS_POLICY_FIFO, 0,
                                    false, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, 0,
                                    nullptr, t);
        TAP_CHECK(entry.valid());
        bool const dead = tx_await(t, KOS_TASK_DEAD);
        int status = -99;
        int const got = kos_task_exit_status(t, &status);
        int const killed = kos_task_kill(t);
        if (killed == 0)
        {
            t = KOS_TASK_NONE;
        }
        TAP_CHECK(dead);
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
#if KICKOS_KERNEL_CORES > 1
    constexpr uint32_t TX_ONE_CORE = 1u;
#else
    constexpr uint32_t TX_ONE_CORE = 0u;
#endif
    // A task with its report, main's reservation in `absent`, which no member of the task
    // reaches; null when either is missing.
#if KICKOS_HAVE_ASPACE
    constexpr RamAsk TX_RAM = {64, ST_GRANULES(1)};
#else
    constexpr RamAsk TX_RAM = {64};
#endif
    TxReport volatile* tx_report_task(kos_task_t* t)
    {
        void* const absent = st_ram<TX_RAM, 0>();
#if KICKOS_HAVE_ASPACE
        size_t const g = arena_granule();
        void* const shared = st_ram<TX_RAM, 1>();
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
        kos_sem_wait(1, KOS_TIMEOUT_NONE); // nothing posts it, and the sibling's fault slays this thread in it
        r->woke = 1;
        kos_yield();
        r->called = 1;
        kos_exit(TX_ENTRY_EXIT);
    }
    void t_task_exit_member_fault()
    {
        TAP_ASK(.workers = 3, .tasks = 1, .sems = 1);
        kos_cap_t go = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        kos::thread::Handle entry;
        ArmHold hold;
        TAP_HOLD(hold.cap(&go) and hold.task(&t) and hold.thread(&entry));
        TxReport volatile* const rep = tx_report_task(&t);
        TAP_CHECK(rep != nullptr);
        TAP_CHECK(kos_sem_create(0, &go) == 0);
        kos_cap_grant const caps[] = {{go, KOS_CAP_WAIT}};
        entry = kos::thread::create(tx_entry_waits, const_cast<TxReport*>(rep), "txe", 10,
                                    KOS_POLICY_FIFO, 0, false, nullptr, 0, nullptr, 0, nullptr, 0,
                                    caps, 1, 0, nullptr, t, TX_ONE_CORE);
        TAP_CHECK(entry.valid());
        bool const dead = tx_await(t, KOS_TASK_DEAD);
        int status = -99;
        int const got = kos_task_exit_status(t, &status);
        int const joined = thread_end(&entry, TX_WAIT_US);
        int const killed = kos_task_kill(t);
        if (killed == 0)
        {
            t = KOS_TASK_NONE;
        }
        TAP_CHECK(dead and rep->spinning == 1);
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
        kos_sem_wait(1, KOS_TIMEOUT_NONE); // nothing posts it: the kill ends this wait
        *static_cast<volatile uint32_t*>(r->absent) = 1u;
        kos_exit(TX_ENTRY_EXIT); // unreachable: the store faults
    }
    void t_task_exit_cancelled_fault()
    {
        TAP_ASK(.workers = 2, .tasks = 1, .sems = 1);
        kos_cap_t go = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        kos::thread::Handle entry;
        ArmHold hold;
        TAP_HOLD(hold.cap(&go) and hold.task(&t) and hold.thread(&entry));
        TxReport volatile* const rep = tx_report_task(&t);
        TAP_CHECK(rep != nullptr);
        TAP_CHECK(kos_sem_create(0, &go) == 0);
        kos_cap_grant const caps[] = {{go, KOS_CAP_WAIT}};
        entry = kos::thread::create(tx_killed_faults, const_cast<TxReport*>(rep), "txf", 10,
                                    KOS_POLICY_FIFO, 0, false, nullptr, 0, nullptr, 0, nullptr, 0,
                                    caps, 1, 0, nullptr, t, TX_ONE_CORE);
        TAP_CHECK(entry.valid());
        uint64_t const give_up = tx_give_up();
        while (rep->spinning == 0 and kos_clock_now() < give_up)
        {
            kos_sleep_ns(TX_POLL_NS);
        }
        int const parked = rep->spinning;
        int const killed = entry.kill();
        int const joined = thread_end(&entry, TX_WAIT_US);
        bool const dead = tx_await(t, KOS_TASK_DEAD);
        int const state = kos_task_state(t);
        int status = -99;
        int const got = kos_task_exit_status(t, &status);
        int const refused = tx_seat(t);
        int const slain = task_end(&t, TX_WAIT_US);
        TAP_CHECK(parked == 1 and killed == 0 and joined == 0);
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
    constexpr RamAsk TXI_RAM = {ST_GRANULES(1)};
    void t_task_exit_implicit_fault()
    {
        TAP_ASK(.workers = 2, .endpoints = 1);
        kos_cap_t ep = KOS_CAP_NONE;
        kos::thread::Handle entry;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.thread(&entry));
        size_t const g = arena_granule();
        void* const own = st_ram<TXI_RAM, 0>();
        void* const absent = st_ram<TX_RAM, 0>();
        TAP_CHECK(own != nullptr and absent != nullptr);
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        kos_cap_grant const caps[] = {{ep, KOS_CAP_WAIT | KOS_CAP_TRANSFER}};
        entry = kos::thread::create(tx_implicit_entry, absent, "txf", 10, KOS_POLICY_FIFO, 0,
                                    false, own, static_cast<uint32_t>(g), nullptr, 0, nullptr, 0,
                                    caps, 1);
        TAP_CHECK(entry.valid());
        int const narrowed = kos_cap_narrow(ep, KOS_CAP_SIGNAL);
        int32_t sent = -99;
        uint64_t const give_up = tx_give_up();
        while (kos_clock_now() < give_up)
        {
            sent = kos_send_timed(ep, "x", 1, 1000u);
            if (sent != -KOS_ETIMEDOUT)
            {
                break;
            }
        }
        int const joined = thread_end(&entry, TX_WAIT_US);
        TAP_CHECK(narrowed == 0 and joined == 0);
        TAP_CHECK(sent == -KOS_ECONNREFUSED);
    }

    // The entry, parked, killed by its spawner and then slain: either death ends the task, so
    // the sibling parked on `go`, which nothing posts, is slain, and the task answers
    // KOS_EXIT_CANCELLED.
    void tx_waits_on_go(void*) // caps: go@1
    {
        kos_sem_wait(1, KOS_TIMEOUT_NONE);
        kos_exit(TX_SIBLING_EXIT);
    }
    void tx_entry_parks(void*) // caps: go@1, park@2, E(SIGNAL)@3
    {
        kos_cap_grant const caps[] = {{1, KOS_CAP_WAIT}};
        (void)kos::thread::create(tx_waits_on_go, nullptr, "txw", 9, KOS_POLICY_FIFO, 0, false,
                                  nullptr, 0, nullptr, 0, nullptr, 0, caps, 1);
        (void)kos_send_timed(3, "r", 1, TX_WAIT_US);
        kos_sem_wait(2, KOS_TIMEOUT_NONE); // nothing posts it: the cancel ends this wait
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
        TAP_ASK(.workers = 2, .tasks = 1, .sems = 2, .endpoints = 1);
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
    void wg_child(void*)
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
    }
    void wg_reader(void*)
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
    }
    void wg_holder(void* arg) // caps: hold@1
    {
        WgHold* const h = static_cast<WgHold*>(arg);
        h->rc = kos_window_get(0, &h->got);
        kos_sem_wait(1, KOS_TIMEOUT_NONE);
    }
    // Spawns a child holding `list`: its handle once it has been joined, else invalid.
    kos::thread::Handle wg_spawn(void (*entry)(void*), kos_window const* list, int n)
    {
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
                                           nullptr, 0, KOS_AUTH_MEMORY);
        if (h.valid() and h.join(STALL_TOLERANT_US) != 0)
        {
            (void)h.slay(ARM_SLAY_US);
            return kos::thread::Handle(KOS_THREAD_NONE, -KOS_ETIMEDOUT);
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
    constexpr RamAsk WG_RAM = {ST_GRANULES(1), ST_GRANULES(2)};
    bool wg_siblings(kos_window const& first)
    {
        void* const blk = st_ram<WG_RAM, 1>();
        kos_cap_t gate = KOS_CAP_NONE;
        kos::thread::Handle h[2];
        ArmHold hold;
        if (blk == nullptr
            or not(hold.cap(&gate) and hold.thread(&h[0]) and hold.thread(&h[1]))
            or kos_sem_create(0, &gate) != 0)
        {
            return false;
        }
        kos_window const second = {reinterpret_cast<uintptr_t>(blk), 2u * first.size,
                                   KOS_WINDOW_MEMORY, 0};
        kos_window const lists[2] = {first, second};
        kos_cap_grant const caps[] = {{gate, KOS_CAP_WAIT}};
        for (int i = 0; i < 2; i++)
        {
            g_wg_hold[i] = {-99, {}};
            h[i] = kos::thread::create(wg_holder, &g_wg_hold[i], "wgh", 10, KOS_POLICY_FIFO, 0,
                                       false, nullptr, 0, nullptr, 0, &lists[i], 1, caps, 1);
        }
        for (int i = 0; i < 2; i++)
        {
            if (h[i].valid())
            {
                (void)kos_sem_post(gate);
            }
        }
        bool ok = h[0].valid() and h[1].valid() and hold.joined();
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
        TAP_ASK(.workers = 2, .sems = 1);
        size_t const g = arena_granule();
        void* const blk = st_ram<WG_RAM, 0>();
        TAP_CHECK(blk != nullptr);
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
            first[0] = {b, WIN, KOS_WINDOW_DEVICE, 0};
            h = wg_spawn(wg_child, first, 2);
            if (h.valid() or h.error() == -KOS_ENOMEM)
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
    // --- A block held non-cacheable, through its grants, IPC and retype -------------------
    // The bytes are what is checked here. The cache maintenance each grant and copy owes is
    // counted on the host (region_sync, alias_sync): no emulated board models a data cache.
    constexpr uint32_t UG_BLK = 64;
    constexpr uint32_t UG_STACK = 4096;
#if KICKOS_MEMORY_ENFORCED
    constexpr RamAsk UG_RAM = {UG_BLK, UG_BLK, UG_BLK, UG_BLK, UG_STACK};
#else
    constexpr RamAsk UG_RAM = {UG_BLK, UG_BLK, UG_BLK, UG_BLK};
#endif
    constexpr size_t UG_LEN = 16;
    // [0] the first block's self-grant, non-cacheable where the board seats one, [1] the
    // second's, cacheable, [2] and [3] the receives into each, [4] the first block's retype to
    // cacheable: 0, the call's refusal, or -1 where a receive delivered other bytes.
    unsigned char* g_ug_blk[2] = {};
    bool g_ug_nocache = false;
    Atomic<int32_t, Order::RELAXED> g_ug_rc[5];
    // A fresh region set of its own, which main's is not by this point of the run.
    void ug_receiver(void*) // caps: E(WAIT)@1
    {
        uint32_t flags[2] = {0, 0};
        if (g_ug_nocache)
        {
            flags[0] = KOS_MEM_NOCACHE;
        }
        for (int i = 0; i < 2; i++)
        {
            g_ug_rc[i] = kos_mem_self_grant(g_ug_blk[i], UG_BLK, flags[i]);
        }
        for (int i = 0; i < 2; i++)
        {
            struct kos_reply_recv_opts o;
            kos_reply_recv_opts_init(&o, 1, KOS_RECV_NO_INFO, STALL_TOLERANT_US);
            int32_t const n =
                kos_reply_recv(KOS_CAP_NONE, g_ug_blk[i], kos_call_lens_pack(0, UG_LEN), &o);
            int32_t rc = n;
            if (n == static_cast<int32_t>(UG_LEN))
            {
                rc = 0;
                for (size_t k = 0; k < UG_LEN; k++)
                {
                    if (g_ug_blk[i][k] != 0x30u + k)
                    {
                        rc = -1;
                    }
                }
            }
            else if (n >= 0)
            {
                rc = -1;
            }
            g_ug_rc[2 + i] = rc;
        }
        g_ug_rc[4] = kos_mem_self_grant(g_ug_blk[0], UG_BLK, 0);
    }
    void ug_noop(void*) {}
#if KICKOS_MEMORY_ENFORCED
    // A child's cacheable stack over a block a thread holds non-cacheable is refused; once that
    // holder is gone the block is a stack again.
    void* g_ug_stk = nullptr;
    int32_t g_ug_stk_rc = 99;
    void ug_stack_worker(void*)
    {
        g_ug_stk_rc = -KOS_EINVAL;
        if (kos_mem_self_grant(g_ug_stk, UG_STACK, KOS_MEM_NOCACHE) == 0)
        {
            auto const child = kos::thread::create(ug_noop, nullptr, "ugs", 10, KOS_POLICY_FIFO,
                                                   0, false, nullptr, 0, g_ug_stk, UG_STACK);
            g_ug_stk_rc = child.error();
            if (child.valid())
            {
                (void)child.join(STALL_TOLERANT_US);
            }
        }
    }
#endif
    void t_uncached_grant_sync()
    {
        TAP_ASK(.workers = 2, .tasks = 1, .endpoints = 1);
        kos_cap_t ep = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        kos::thread::Handle holder;
        kos::thread::Handle receiver;
        kos::thread::Handle worker;
        kos::thread::Handle child;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.task(&t) and hold.thread(&holder)
                 and hold.thread(&receiver) and hold.thread(&worker) and hold.thread(&child));
        g_ug_blk[0] = static_cast<unsigned char*>(st_ram<UG_RAM, 0>());
        g_ug_blk[1] = static_cast<unsigned char*>(st_ram<UG_RAM, 1>());
        void* const win = st_ram<UG_RAM, 2>();
        void* const hand = st_ram<UG_RAM, 3>();
        TAP_CHECK(g_ug_blk[0] != nullptr and g_ug_blk[1] != nullptr and win != nullptr
                  and hand != nullptr);
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
        g_ug_nocache = KICKOS_HAVE_MPU != 0;
        if (g_ug_nocache)
        {
            kos_window const w = {reinterpret_cast<uintptr_t>(win), UG_BLK, KOS_WINDOW_MEMORY,
                                  KOS_WINDOW_UNCACHED};
            holder = kos::thread::create(ug_noop, nullptr, "ugw", 10, KOS_POLICY_FIFO, 0, false,
                                         nullptr, 0, nullptr, 0, &w, 1, nullptr, 0);
            int const held = holder.error();
            bool const gone = hold.joined();
            holder = kos::thread::Handle();
            int const trc = kos_task_create(hand, UG_BLK, KOS_MEM_NOCACHE, &t);
            tap::diag("uncached window: %ld, task data: %ld", static_cast<long>(held),
                      static_cast<long>(trc));
            TAP_CHECK(held == 0 and gone);
            TAP_CHECK(trc == 0);
            TAP_CHECK(kos_task_kill(t) == 0);
            t = KOS_TASK_NONE;
        }
        for (int i = 0; i < 5; i++)
        {
            g_ug_rc[i] = -99;
        }
        unsigned char msg[UG_LEN];
        for (size_t i = 0; i < UG_LEN; i++)
        {
            msg[i] = static_cast<unsigned char>(0x30u + i);
        }
        kos_cap_grant const caps[] = {{ep, KOS_CAP_WAIT}};
        receiver = kos::thread::create_caps(ug_receiver, nullptr, "ugr", TAP_PRIO_PARKS, caps, 1,
                                            KOS_POLICY_FIFO, 0, false, nullptr, 0,
                                            KOS_AUTH_MEMORY, nullptr, KOS_TASK_NONE, nullptr, 0,
                                            TAP_PIN_CORE);
        TAP_CHECK(receiver.valid());
        kos_yield();
        int32_t sent[2] = {-1, -1};
        for (int i = 0; i < 2; i++)
        {
            sent[i] = kos_send_timed(ep, msg, UG_LEN, STALL_TOLERANT_US);
        }
        TAP_CHECK(hold.joined());
        receiver = kos::thread::Handle();
        TAP_CHECK(hold.close(&ep) == 0);
        tap::diag("self-grant %ld, cacheable %ld, recv %ld, cacheable recv %ld, "
                  "retype to cacheable %ld",
                  static_cast<long>(g_ug_rc[0].load()), static_cast<long>(g_ug_rc[1].load()),
                  static_cast<long>(g_ug_rc[2].load()), static_cast<long>(g_ug_rc[3].load()),
                  static_cast<long>(g_ug_rc[4].load()));
        TAP_CHECK(sent[0] == static_cast<int32_t>(UG_LEN)
                  and sent[1] == static_cast<int32_t>(UG_LEN));
        TAP_CHECK(g_ug_rc[0].load() == 0 and g_ug_rc[1].load() == 0);
        TAP_CHECK(g_ug_rc[2].load() == 0 and g_ug_rc[3].load() == 0);
        TAP_CHECK(g_ug_rc[4].load() == 0);
#if KICKOS_MEMORY_ENFORCED
        if (not g_ug_nocache)
        {
            return;
        }
        g_ug_stk = st_ram<UG_RAM, 4>();
        worker = kos::thread::create(ug_stack_worker, nullptr, "ugk", TAP_PRIO_PARKS,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, nullptr, 0, nullptr,
                                     0, nullptr, 0, KOS_AUTH_MEMORY);
        TAP_CHECK(worker.valid());
        TAP_CHECK(hold.joined());
        worker = kos::thread::Handle();
        child = kos::thread::create(ug_noop, nullptr, "ugs", 10, KOS_POLICY_FIFO, 0, false,
                                    nullptr, 0, g_ug_stk, UG_STACK);
        int32_t const again = child.error();
        bool const rejoined = child.valid() and hold.joined();
        child = kos::thread::Handle();
        tap::diag("a stack over a block held non-cacheable: %ld, once its holder is gone: %ld",
                  static_cast<long>(g_ug_stk_rc), static_cast<long>(again));
        TAP_CHECK(g_ug_stk_rc == -KOS_EBUSY);
        TAP_CHECK(again == 0 and rejoined);
        // The same block again, for a stack inside the spawn's own uncached window and then one
        // over the non-cacheable data of the task it joins.
        kos_window const uw = {reinterpret_cast<uintptr_t>(g_ug_stk), UG_STACK, KOS_WINDOW_MEMORY,
                               KOS_WINDOW_UNCACHED};
        child = kos::thread::create(ug_noop, nullptr, "ugv", 10, KOS_POLICY_FIFO, 0, false,
                                    nullptr, 0, g_ug_stk, UG_STACK, &uw, 1);
        int32_t const over_window = child.error();
        tap::diag("inside its own uncached window: %ld", static_cast<long>(over_window));
        TAP_CHECK(over_window == -KOS_EBUSY);
        int const task_rc = kos_task_create(g_ug_stk, UG_STACK, KOS_MEM_NOCACHE, &t);
        if (task_rc != 0)
        {
            tap::partial("no task to hold the block as its non-cacheable data (rc %d)", task_rc);
            return;
        }
        child = kos::thread::create(ug_noop, nullptr, "ugd", 10, KOS_POLICY_FIFO, 0, false,
                                    nullptr, 0, g_ug_stk, UG_STACK, nullptr, 0, nullptr, 0, 0,
                                    nullptr, t);
        int32_t const over_data = child.error();
        tap::diag("over its task's non-cacheable data: %ld", static_cast<long>(over_data));
        TAP_CHECK(over_data == -KOS_EBUSY);
#endif
    }
#endif

    // A member's memory is its TASK's, so a member bringing its own data grant is refused
    // rather than having it silently dropped; and a task nobody created cannot be joined.
    constexpr RamAsk TMR_RAM = {64};
    void t_task_member_refusals()
    {
        kos_task_t task = KOS_TASK_NONE;
        ArmHold hold;
        TAP_HOLD(hold.task(&task));
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &task) == 0);
        void* const blk = st_ram<TMR_RAM, 0>();
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
        task = KOS_TASK_NONE;
    }

    // One member parks on a semaphore nothing posts, the other spins in its own code. The arm
    // shows that the spinner ran, and that after the kill both joined and the member did not
    // report again.
    void task_member(void*) // caps: E(SIGNAL)@1, park@2
    {
        tx_report_spun(1);
        kos_sem_wait(2, KOS_TIMEOUT_NONE);
        (void)kos_sem_post(2);
        kos_exit(1);
    }

    void t_task_group_kill()
    {
        TAP_ASK(.workers = 2, .tasks = 1, .sems = 1, .endpoints = 1);
        kos_cap_t park = KOS_CAP_NONE;
        kos_cap_t ready = KOS_CAP_NONE;
        kos_task_t task = KOS_TASK_NONE;
        kos::thread::Handle member;
        kos::thread::Handle spinner;
        ArmHold hold;
        TAP_HOLD(hold.cap(&park) and hold.cap(&ready) and hold.task(&task)
                 and hold.thread(&member) and hold.thread(&spinner));
        TAP_CHECK(kos_sem_create(0, &park) == 0);
        TAP_CHECK(kos_endpoint_create(&ready) == 0);
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &task) == 0);
        kos_cap_grant const caps[2] = {{ready, KOS_CAP_SIGNAL}, {park, CH_FULL}};
        g_tx_turns = 0;
        member = kos::thread::create_caps(task_member, nullptr, "tmbr", TAP_PRIO_PARKS, caps, 2,
                                          KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr, task,
                                          nullptr, 0, TAP_PIN_CORE);
        spinner = kos::thread::create_caps(tx_spins, nullptr, "tspn", KICKOS_PRIO_MIN,
                                           nullptr, 0, KOS_POLICY_FIFO, 0, false, nullptr, 0, 0,
                                           nullptr, task, nullptr, 0, TAP_PIN_CORE);
        TAP_CHECK(member.valid() and spinner.valid());
        char spun = 0;
        TAP_CHECK(report_await(ready, &spun, 1));
        int const killed = kos_task_kill(task);
        if (killed == 0)
        {
            task = KOS_TASK_NONE;
        }
        // 0, not ETIMEDOUT: each member has to be GONE, not just marked.
        int const member_joined = thread_end(&member, TX_WAIT_US);
        int const spinner_joined = thread_end(&spinner, TX_WAIT_US);
        bool const woke = kos_sem_wait(park, 0) == 0;
        TAP_CHECK(killed == 0);
        TAP_CHECK(spinner_joined == 0);
        TAP_CHECK(member_joined == 0 and not woke);
        if (spun != 's')
        {
            TAP_SKIP_VACUOUS("the spinner never ran its own code before the kill");
            return;
        }
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
        TAP_ASK(.workers = 1, .tasks = 1);
        kos_task_t t = KOS_TASK_NONE;
        kos::thread::Handle m;
        ArmHold hold;
        TAP_HOLD(hold.task(&t) and hold.thread(&m));
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
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &t) == 0);
        int const granted = kos_task_sched_grant(t, SC_CEILING, 0);
        m = kos::thread::create_caps(sc_worker, nullptr, "scself", SC_START, nullptr, 0,
                                     KOS_POLICY_FIFO, 0, false, nullptr, 0, 0, nullptr, t);
        TAP_CHECK(m.valid());
        TAP_CHECK(hold.joined());
        int wrong = -1;
        int const read = kos_task_exit_status(t, &wrong);
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
        kos_sem_wait(2, KOS_TIMEOUT_NONE);
        g_slay_window = g_slay_window + 1;
        kos_exit(0); // never returns: the kernel ends the thread at this syscall's entry
    }

    // Without the park both arms below pass vacuously, the worker having died at the entry to a
    // wait it never reached.
    bool stage_a_parked_slay_worker(kos::thread::Handle* out, kos_cap_t park)
    {
        kos_cap_grant const caps[2] = {{g_done, CH_FULL}, {park, CH_FULL}};
        *out = kos::thread::create_caps(slay_window_worker, nullptr, "slay", 10, caps, 2);
        if (not out->valid())
        {
            return false;
        }
        return kos_sem_wait(g_done, STALL_TOLERANT_US) == 0;
    }

    // How many times leg 1 may restage. The worker posts before it parks, and above one
    // kernel core a kill can land in that gap, ending it at its next syscall entry with no
    // window at all. That is a staging that did not take, not a failure of the property.
    constexpr int SLAY_STAGE_TRIES = 8;

    void t_thread_slay_window()
    {
        TAP_ASK(.workers = 1, .sems = 1);
        kos_cap_t park = KOS_CAP_NONE;
        kos::thread::Handle killed;
        kos::thread::Handle slain;
        ArmHold hold;
        TAP_HOLD(hold.cap(&park) and hold.thread(&killed) and hold.thread(&slain));
        TAP_CHECK(kos_sem_create(0, &park) == 0);
        // LEG 1, the control. A kill breaks the park and the target returns to userspace for
        // exactly as long as it takes to reach its next syscall.
        int slay_window = 0;
        for (int attempt = 0; attempt < SLAY_STAGE_TRIES and slay_window != 1; attempt++)
        {
            TAP_CHECK(stage_a_parked_slay_worker(&killed, park));
            g_slay_window = 0;
            TAP_CHECK(killed.kill() == 0);
            TAP_CHECK(thread_end(&killed) == 0);
            slay_window = g_slay_window;
        }
        // The window is the machine's to grant: every restage lost the race to main's kill.
        if (slay_window != 1)
        {
            TAP_SKIP_VACUOUS("the kill window never opened in %d stagings", SLAY_STAGE_TRIES);
            return;
        }

        // LEG 2, the subject. Same body, same park, same parent: only the verb differs.
        TAP_CHECK(stage_a_parked_slay_worker(&slain, park));
        // 0, not -KOS_ETIMEDOUT: the call WAITS, and gone is what it returns.
        //
        // The claim holds on either interleaving, which is why this leg needs no restaging.
        // A slay reaching a victim already parked aborts the park and switch_to redirects its
        // resume; one reaching a victim still short of that park is found at the victim's own
        // syscall entry. Neither lets it run a further user instruction, and above one kernel
        // core both are reachable.
        TAP_CHECK(slain.slay(STALL_TOLERANT_US) == 0);
        int const slay_window_after = g_slay_window;
        TAP_CHECK(slay_window_after == 1); // unchanged: it executed no further user instruction
        // Gone means gone, and the join is the independent witness of it.
        TAP_CHECK(kos_thread_join(slain.id(), JOIN_GENEROUS_US) == 0);
        slain = kos::thread::Handle();
        TAP_CHECK(hold.close(&park) == 0);
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
    }

    void t_thread_slay_gate()
    {
        TAP_ASK(.workers = 1);
        log_reset();
        kos::thread::Handle probe;
        kos::thread::Handle s;
        ArmHold hold;
        TAP_HOLD(hold.thread(&probe) and hold.thread(&s));
        TAP_CHECK(kos_thread_slay(KOS_THREAD_NONE, JOIN_GENEROUS_US) == -KOS_EBADF);
        TAP_CHECK(kos_thread_slay(0x7fffffffu, JOIN_GENEROUS_US) == -KOS_EBADF);
        // Not -KOS_EDEADLK as join answers: ending yourself is kos_exit, and this call has
        // to be able to return to its caller.
        TAP_CHECK(kos_thread_slay(g_main, JOIN_GENEROUS_US) == -KOS_EINVAL);
        // An EXITED-but-unreclaimed slot, which join deliberately ACCEPTS and every cancel
        // deliberately refuses: there is nothing left in it to condemn.
        probe = kos::thread::create(join_probe, nullptr, "slgn", 10);
        TAP_CHECK(probe.valid());
        kos_thread_t const exited = probe.id();
        TAP_CHECK(thread_end(&probe) == 0);
        TAP_CHECK(kos_thread_slay(exited, JOIN_GENEROUS_US) == -KOS_EBADF);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        s = kos::thread::create_caps(slay_gate_probe, nullptr, "slst", 10, caps, 2);
        TAP_CHECK(s.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(log_eq("P")); // parenthood, and there is no capability to delegate it with
    }

    // --- Slay: the group form ---------------------------------------------------
    // 0 here means a condition no other call in the ABI waits on: the group is EMPTY.
    void t_task_slay_group()
    {
        TAP_ASK(.workers = 1, .tasks = 1, .sems = 1);
        kos_cap_t park = KOS_CAP_NONE;
        kos_task_t task = KOS_TASK_NONE;
        kos::thread::Handle member;
        ArmHold hold;
        TAP_HOLD(hold.cap(&park) and hold.task(&task) and hold.thread(&member));
        TAP_CHECK(kos_sem_create(0, &park) == 0);
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &task) == 0);
        g_slay_window = 0;
        kos_cap_grant const caps[2] = {{g_done, CH_FULL}, {park, CH_FULL}};
        member = kos::thread::create(slay_window_worker, nullptr, "tsly", 10, KOS_POLICY_FIFO, 0,
                                     /*privileged=*/false, nullptr, 0, nullptr, 0, nullptr, 0,
                                     caps, 2, /*authority=*/0, /*cap_dest=*/nullptr, task);
        TAP_CHECK(member.valid());
        TAP_CHECK(kos_sem_wait(g_done, STALL_TOLERANT_US) == 0);
        kos_task_t const slain = task;
        TAP_CHECK(task_end(&task) == 0);
        int const slay_window = g_slay_window;
        TAP_CHECK(slay_window == 0);          // no member got a cleanup window
        TAP_CHECK(thread_end(&member, JOIN_GENEROUS_US) == 0); // and the member really is gone
        // The hold went with the wait, so the handle names nothing: a second call cannot
        // resolve it, which is the same shape kos_task_kill leaves behind.
        TAP_CHECK(kos_task_slay(slain, JOIN_GENEROUS_US) == -KOS_EBADF);
        TAP_CHECK(hold.close(&park) == 0);
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
    }

    void t_task_slay_gate()
    {
        TAP_ASK(.workers = 1, .tasks = 1);
        log_reset();
        kos_task_t task = KOS_TASK_NONE;
        kos::thread::Handle s;
        ArmHold hold;
        TAP_HOLD(hold.task(&task) and hold.thread(&s));
        TAP_CHECK(kos_task_slay(KOS_TASK_NONE, JOIN_GENEROUS_US) == -KOS_EBADF);
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &task) == 0);
        kos_cap_grant caps[] = {{g_done, CH_FULL}, {g_lock, CH_FULL}};
        s = kos::thread::create_caps(task_slay_stranger,
                                     reinterpret_cast<void*>(static_cast<uintptr_t>(task)), "tsst",
                                     10, caps, 2);
        TAP_CHECK(s.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(log_eq("P")); // creatorship, exactly as kos_task_kill takes it
        // An EMPTY group answers 0 with nothing to slay: dropping the creator's hold is the
        // whole of the work, and a park here would never be woken.
        kos_task_t const slain = task;
        TAP_CHECK(task_end(&task) == 0);
        TAP_CHECK(kos_task_slay(slain, JOIN_GENEROUS_US) == -KOS_EBADF);
    }

    // --- A slay returns once EVERY member's teardown is done ---------------------------------
    // The entry returns at once and its end cancels the sibling, which leaves at its first
    // system call. The sibling holds the only receive right of the endpoint the creator calls
    // in its first slot, and of the probe a watcher calls in its last. Its sweep ends the
    // creator's call first, and the creator slays the task in the middle of that sweep, the task
    // empty and still sweeping. The watcher's call ends when the sweep reaches the last slot, so
    // its stamp has to come before the slay's return, and the restart takes the slot back.
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
    void ts_creator(void*)
    {
        uint64_t const until = deadline_in(CREATOR_US);
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
                g_ts.called = kos_call_timed(ep, b, sizeof(b), sizeof(b), left_us_until(until));
                g_ts.slain = kos_task_slay(t, left_us_until(until));
                g_ts_seq = g_ts_seq + 1;
                g_ts.slay_seq = g_ts_seq;
                kos_task_t again = KOS_TASK_NONE;
                g_ts.restarted = kos_task_create(nullptr, 0, 0, &again);
                g_ts.same_slot = ((again ^ t) & 0xFFFFu) == 0u;
                (void)kos_task_kill(again);
            }
            if (watcher.valid())
            {
                (void)watcher.join(left_us_until(until));
            }
            if (entry.valid())
            {
                (void)entry.join(left_us_until(until));
            }
            if (sibling.valid())
            {
                (void)sibling.join(left_us_until(until));
            }
        }
        if (g_ts.slain != 0)
        {
            (void)kos_task_kill(t);
        }
        (void)kos_handle_close(probe);
        (void)kos_handle_close(ep);
    }
    void t_task_slay_after_every_sweep()
    {
        // The creator's task and endpoints come out of main's task budgets.
        TAP_ASK(.workers = 4, .tasks = 1, .endpoints = 2);
        g_ts = {-99, -99, -99, 0, 0, -99, 0, -99, -99, -99, -99};
        g_ts_seq = 0;
        kos::thread::Handle w;
        ArmHold hold(CREATOR_HOLD_US);
        TAP_HOLD(hold.thread(&w));
        w = kos::thread::create(ts_creator, nullptr, "tsc", 14, KOS_POLICY_FIFO, 0, false, nullptr,
                                0, nullptr, 0, nullptr, 0, nullptr, 0, KOS_AUTH_TASKS, nullptr,
                                KOS_TASK_NONE, 1u);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
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
            // Not yet run is the healthy case and the opposite of spent: the whole window is still
            // ahead.
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
        // The instrument is starvation: one hog outranking the victim denies it THE core, so the
        // slay's wait expires while the death stays owed. Above one core the placement of N spinners
        // is the scheduler's to choose.
        TAP_SKIP_ONE_CORE_ORDER();
        TAP_ASK(.workers = 2, .sems = 1);
        kos_cap_t park = KOS_CAP_NONE;
        kos::thread::Handle victim;
        kos::thread::Handle hog;
        ArmHold hold;
        TAP_HOLD(hold.cap(&park) and hold.thread(&victim) and hold.thread(&hog));
        TAP_CHECK(kos_sem_create(0, &park) == 0);
        g_slay_window = 0;
        TAP_CHECK(stage_a_parked_slay_worker(&victim, park));
        // Spawned AFTER the victim is staged: a spawn is not a barrier and this one would otherwise
        // hog the staging itself.
        g_hog_start_ns = 0; // a previous arm's window must not read as this one's
        hog = kos::thread::create(slay_hog, nullptr, "shog", 11);
        TAP_CHECK(hog.valid());
        // Condemned, not gone: the redirect is armed and irrevocable, and the sweep has not
        // run because the victim has not been given the CPU to run it on. The window runs
        // from the hog's first run and the caller can lose all of it between the two: under
        // an interrupt-driven console the caller blocks there, the hog spends its window and
        // the victim dies at once. slay returning 0 is then correct, so asserting the timeout
        // without this check measures nothing.
        for (int attempt = 0; attempt < 2 and not hog_window_open(); attempt++)
        {
            TAP_CHECK(thread_end(&hog) == 0);
            g_hog_start_ns = 0;
            hog = kos::thread::create(slay_hog, nullptr, "shog", 11);
            TAP_CHECK(hog.valid());
        }
        if (not hog_window_open())
        {
            TAP_SKIP_VACUOUS("hog window spent before the slay, starvation not established");
            return;
        }
        TAP_CHECK(victim.slay(SLAY_TIMEOUT_US) == -KOS_ETIMEDOUT);
        int const slay_window = g_slay_window;
        TAP_CHECK(slay_window == 0); // and it never got its window either
        // Irrevocable is the claim, so the same handle must reach GONE with no second
        // request: the timeout gave up on the wait, never on the death.
        TAP_CHECK(kos_thread_join(victim.id(), STALL_TOLERANT_US) == 0);
        int const slay_window_after_join = g_slay_window;
        TAP_CHECK(slay_window_after_join == 0);
        TAP_CHECK(hold.joined());
        TAP_CHECK(hold.close(&park) == 0);
    }

    // --- Self-grant, and the region budget that bounds it ----------------------
    // Exercises the REFUSAL at the region-budget ceiling: the call MUST fail loudly
    // (-KOS_ENOMEM) and never truncate the region set. Runs in an unprivileged CHILD: a
    // privileged caller's self-grants are answered "already reachable" without spending a
    // descriptor, so the ceiling would be unreachable. Each descriptor is bought with a
    // one-byte block, the smallest region this backend can describe.
    constexpr int SG_MAX = 12; // > KICKOS_MPU_MAX_REGIONS, so the loop must end refused
    struct SgReport
    {
        int32_t ok;       // descriptors accepted before the ceiling
        int32_t refusal;  // the code that ended the loop
        int32_t badsize;
        int32_t readback;
    };
    // The ceiling is a different one where a backend translates. No descriptor is seated
    // there, so the self-grant spends no region budget; what runs out is the RESERVATION
    // list, which allocation spends one slot of. The child is then a task of its own, whose
    // list its end gives back, and it takes one block more than its space has slots left.
#if KICKOS_HAVE_ASPACE
#define SG_ASK ST_NO_RAM
    int64_t sg_count(uint32_t which)
    {
        uint32_t n = 0;
        if (kos_mem_count(which, &n) != 0)
        {
            return -1;
        }
        return n;
    }
    int sg_budget()
    {
        int64_t const free_slots = sg_count(KOS_MEM_RANGES_FREE);
        if (free_slots < 0)
        {
            return -1;
        }
        return static_cast<int>(free_slots) + 1;
    }
    // Where no block is left: -KOS_ENOMEM at the reservation ceiling, frames still free.
    int32_t sg_out_of_blocks()
    {
        int64_t const ranges = sg_count(KOS_MEM_RANGES_FREE);
        int64_t const frames = sg_count(KOS_MEM_FRAMES_FREE);
        if (ranges == 0 and frames > 0)
        {
            return -KOS_ENOMEM;
        }
        return -KOS_EINVAL;
    }
#else
    constexpr RamAsk SG_RAM = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
#define SG_ASK SG_RAM
    static_assert(sizeof(SG_RAM.size) / sizeof(SG_RAM.size[0]) == SG_MAX);
    int sg_budget()
    {
        return SG_MAX;
    }
    int32_t sg_out_of_blocks()
    {
        return -KOS_EINVAL;
    }
#endif
    void* sg_block(int i)
    {
#if KICKOS_HAVE_ASPACE
        (void)i;
        return st_ram_own(1);
#else
        return st_ram_blocks<SG_RAM>[i];
#endif
    }

    void selfgrant_worker(void*) // caps: E(SIGNAL)@1
    {
        SgReport rep = {0, 0, 0, -1};
        // Size-0 refusal costs no descriptor. The address is a valid stack local, so the
        // refusal is about the SIZE alone.
        int probe = 0;
        rep.badsize = kos_mem_self_grant(&probe, 0, 0);
        int const budget = sg_budget();
        if (budget < 0)
        {
            rep.refusal = -KOS_EINVAL;
        }
        for (int i = 0; i < budget; i++)
        {
            void* const p = sg_block(i);
            if (p == nullptr)
            {
                rep.refusal = sg_out_of_blocks();
                break;
            }
            int32_t const rc = kos_mem_self_grant(p, 1, 0);
            if (rc != 0)
            {
                rep.refusal = rc;
                break;
            }
            // Touch the block just granted: an ungranted write from an unprivileged
            // thread faults, so reaching the readback is the positive half.
            *static_cast<volatile int*>(p) = 0x5A5A + i;
            rep.readback = *static_cast<volatile int*>(p) - i;
            rep.ok = rep.ok + 1;
        }
        (void)kos_send(1, &rep, sizeof(rep));
    }
    void t_selfgrant()
    {
#if KICKOS_HAVE_ASPACE
        TAP_ASK(.workers = 1, .tasks = 1, .endpoints = 1);
#else
        TAP_ASK(.workers = 1, .endpoints = 1);
#endif
        kos_cap_t ep = KOS_CAP_NONE;
        kos_task_t t = KOS_TASK_NONE;
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.cap(&ep) and hold.task(&t) and hold.thread(&w));
        TAP_CHECK(kos_endpoint_create(&ep) == 0);
#if KICKOS_HAVE_ASPACE
        TAP_CHECK(kos_task_create(nullptr, 0, 0, &t) == 0);
#endif
        kos_cap_grant const caps[] = {{ep, KOS_CAP_SIGNAL}};
        w = kos::thread::create_caps(selfgrant_worker, nullptr, "sgW", 10, caps, 1,
                                     KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr, 0,
                                     /*authority=*/KOS_AUTH_MEMORY, nullptr, t);
        TAP_CHECK(w.valid());
        SgReport rep = {-1, -1, -1, -1};
        TAP_CHECK(report_await(ep, &rep, sizeof(rep)));
        TAP_CHECK(hold.joined());
        if (t != KOS_TASK_NONE)
        {
            TAP_CHECK(task_end(&t) == 0);
        }
        TAP_CHECK(rep.ok > 0 and rep.refusal == -KOS_ENOMEM);
        TAP_CHECK(rep.readback == 0x5A5A and rep.badsize == -KOS_EINVAL);
    }

    // Self-grant three-granule blocks on backends that allow non-power-of-two
    // regions. Two blocks reserved back to back cannot both sit on four granules,
    // exposing an invalid size-minus-one alignment check. Bitmap allocators may
    // return aligned bases, so they test the size but not this alignment case.
    Atomic<int32_t, Order::RELAXED> g_sgnp_rc{-1};
    Atomic<int, Order::RELAXED> g_sgnp_ran{0};
    constexpr RamAsk SGNP_RAM = {ST_GRANULES(3), ST_GRANULES(3)};
    void sgnp_worker(void*)
    {
        size_t const g = arena_granule();
        size_t const want = 3u * g;
        void* pick = st_ram<SGNP_RAM, 0>();
        if ((reinterpret_cast<uintptr_t>(pick) & (4u * g - 1u)) == 0)
        {
            pick = st_ram<SGNP_RAM, 1>();
        }
        if (pick != nullptr)
        {
            g_sgnp_rc = kos_mem_self_grant(pick, want, 0);
            g_sgnp_ran = 1;
        }
    }
    // --- Which region-encoding mode is live on this board ----------------------
    // The bump allocator's step for a 3-granule request IS the mode: a base+limit backend
    // reserves 3 granules, a pow2 backend rounds to 4. Two equal blocks of one ask are reserved
    // back to back, and so are two one-byte ones, whose step is the granule. A bump arena only:
    // under translation reservations come out of a first-fit bitmap, whose order is not public
    // API, so the arm is not registered.
#if not KICKOS_HAVE_ASPACE
    constexpr RamAsk RM_RAM = {ST_GRANULES(3), ST_GRANULES(3), 1, 1};
    void t_region_mode()
    {
        void* const p = st_ram<RM_RAM, 0>();
        void* const q = st_ram<RM_RAM, 1>();
        void* const u = st_ram<RM_RAM, 2>();
        void* const v = st_ram<RM_RAM, 3>();
        TAP_CHECK(p != nullptr and q != nullptr and u != nullptr and v != nullptr);
        size_t const g = static_cast<size_t>(reinterpret_cast<uintptr_t>(v)
                                             - reinterpret_cast<uintptr_t>(u));
        TAP_CHECK(g == arena_granule());
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
        TAP_ASK(.workers = 1);
        kos::thread::Handle w;
        ArmHold hold;
        TAP_HOLD(hold.thread(&w));
        w = kos::thread::create(sgnp_worker, nullptr, "sgNP", 10, KOS_POLICY_FIFO, 0,
                                /*privileged=*/false, nullptr, 0, nullptr, 0, nullptr, 0, nullptr,
                                0, /*authority=*/KOS_AUTH_MEMORY);
        TAP_CHECK(w.valid());
        TAP_CHECK(hold.joined());
        TAP_CHECK(g_sgnp_ran == 1);
        int32_t const sgnp_rc = g_sgnp_rc;
        TAP_CHECK(sgnp_rc == 0);
    }
}

namespace selftest
{
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
}

namespace
{
    // A failing arm can return with the shared g_ep still open.
    void after_failure()
    {
        (void)kos_handle_close(g_ep);
        g_ep = KOS_CAP_NONE;
        done_drain();
    }
}

extern "C" void selftest_main(kos_self_t const* self)
{
    g_self = self;
    g_main = kos_thread_self();
    kos_sem_create(1, &g_lock);
    kos_sem_create(0, &g_done);
    tap::set_after_failure(after_failure);
    tap::add_census(cap_census, "main's free capability slots");
#if KICKOS_HAVE_ASPACE
    tap::add_census(spaces_census, "address spaces held");
    tap::add_census(frames_census, "frames free");
    tap::add_census(ranges_census, "main's free range slots");
#endif

// Region 1: everything down to the #undef below. A boundary only ever moves between two
// ADJACENT registrations, so no arm changes place relative to another. One TAP_ADD per line,
// under #if guards alone.
#if KICKOS_SELFTEST_REGION(1)
#define TAP_ADD(name, ...) TAP_ADD_LIVE(name, __VA_ARGS__)
#else
#define TAP_ADD(name, ...) TAP_ADD_ELIDED(name, __VA_ARGS__)
#endif
    // Core scheduler / sync / time: no test-only syscalls, runs on every board.
    TAP_ADD("svc_roundtrip", t_svc);
    TAP_ADD("fifo_order", t_fifo);
    TAP_ADD("preempt_on_ready", t_preempt);
    TAP_ADD("periph_clock_hz", t_periph_clock_hz);
    TAP_ADD("cap_dest", t_cap_dest);
    TAP_ADD("cpu_clock_set", t_cpu_clock_set);
    TAP_ADD("rr_interleave", t_rr);
    TAP_ADD("sleep_order", t_sleep);
    TAP_ADD_PINNED("multi_wait", t_multi);
    TAP_ADD("sem_destroy", t_sem_destroy);
    TAP_ADD_PINNED("sem_destroy_quiescent", t_sem_destroy_busy);
    TAP_ADD("sem_raii", t_sem_raii);
    TAP_ADD_PINNED("sem_timed_wait", t_sem_timed);
    // PI-mutex capability: production syscalls only, so runs on every board.
    TAP_ADD("mutex_basic", t_mutex_basic);
    TAP_ADD("mutex_pi_donation", t_mutex_pi);
#undef TAP_ADD
// Region 2.
#if KICKOS_SELFTEST_REGION(2)
#define TAP_ADD(name, ...) TAP_ADD_LIVE(name, __VA_ARGS__)
#else
#define TAP_ADD(name, ...) TAP_ADD_ELIDED(name, __VA_ARGS__)
#endif
    TAP_ADD("mutex_chain_boost", t_mutex_chain);
    TAP_ADD_PINNED("mutex_owner_died", t_mutex_owner_died);
    TAP_ADD("mutex_deadlock", t_mutex_deadlock);
    TAP_ADD("mutex_close_owned", t_mutex_close_owned);
    TAP_ADD("mutex_multi_held", t_mutex_multi_held);
    TAP_ADD("mutex_unlock_errors", t_mutex_unlock_errors);
    TAP_ADD("mutex_owner_died_nowaiter", t_mutex_owner_died_nowaiter);
    TAP_ADD("mutex_deleg_refcount", t_mutex_deleg_refcount);
    TAP_ADD("prio_self_raise_lower", t_prio_self_raise_lower);
    TAP_ADD("prio_self_boosted", t_prio_self_boosted);
    // Endpoint IPC: production syscalls, so runs on every board.
    TAP_ADD_PINNED("endpoint_rendezvous", t_endpoint_rendezvous);
    TAP_ADD("endpoint_reject", t_endpoint_reject);
    TAP_ADD("endpoint_rights", t_endpoint_rights);
    TAP_ADD_PINNED("endpoint_handout", t_endpoint_handout);
#undef TAP_ADD
// Region 3.
#if KICKOS_SELFTEST_REGION(3)
#define TAP_ADD(name, ...) TAP_ADD_LIVE(name, __VA_ARGS__)
#else
#define TAP_ADD(name, ...) TAP_ADD_ELIDED(name, __VA_ARGS__)
#endif
    TAP_ADD_PINNED("endpoint_handout_parked", t_endpoint_handout_parked);
    TAP_ADD("endpoint_zero_accept", t_endpoint_zero_accept);
    TAP_ADD("endpoint_send_timeout", t_endpoint_send_timeout);
    TAP_ADD("recv_timeout", t_recv_timeout);
    TAP_ADD("timed_arg_refusals", t_timed_arg_refusals);
    TAP_ADD("call_timeout_pending", t_call_timeout_pending);
    TAP_ADD("call_timeout_revert", t_call_timeout_revert);
    TAP_ADD_PINNED("call_timeout_reply", t_call_timeout_reply);
    TAP_ADD_PINNED("reply_stale_caller", t_reply_stale_caller);
    TAP_ADD("reply_abandoned_cap", t_reply_abandoned_cap);
    TAP_ADD("call_infoless_revert", t_call_infoless_revert);
    TAP_ADD("call_close_reply", t_call_close_reply);
#undef TAP_ADD
// Region 4.
#if KICKOS_SELFTEST_REGION(4)
#define TAP_ADD(name, ...) TAP_ADD_LIVE(name, __VA_ARGS__)
#else
#define TAP_ADD(name, ...) TAP_ADD_ELIDED(name, __VA_ARGS__)
#endif
    TAP_ADD_PINNED("reply_recv_loop", t_reply_recv_loop);
    TAP_ADD_PINNED("reply_recv_bad_ep_wakes_caller", t_reply_recv_bad_ep_wakes_caller);
    TAP_ADD_PINNED("service_survives_client_fault", t_service_survives_client_fault);
#if defined(KICKOS_ENABLE_SELFTEST)
    TAP_ADD_IRQ("reply_recv_notify_park", t_reply_recv_notify_park);
    TAP_ADD_IRQ("reply_recv_notify", t_reply_recv_notify);
    TAP_ADD_IRQ("irq_wait_timeout", t_irq_wait_timeout);
#endif
#if defined(KICKOS_ENABLE_SELFTEST)
    TAP_ADD_PINNED("call_reg_fastpath", t_call_reg_fastpath);
#endif
    TAP_ADD_PINNED("call_from_root", t_call_from_root);
    TAP_ADD_PINNED("call_truncation", t_call_truncation);
    TAP_ADD_PINNED("call_prepop_death", t_call_prepop_death);
#undef TAP_ADD
// Region 5.
#if KICKOS_SELFTEST_REGION(5)
#define TAP_ADD(name, ...) TAP_ADD_LIVE(name, __VA_ARGS__)
#else
#define TAP_ADD(name, ...) TAP_ADD_ELIDED(name, __VA_ARGS__)
#endif
    TAP_ADD("call_donation_hold", t_call_donation_hold);
    TAP_ADD("call_donation_slow", t_call_donation_slow);
    TAP_ADD("call_donation_pending", t_call_donation_pending);
    TAP_ADD("endpoint_crossdomain", t_endpoint_crossdomain, XD_RAM);
#if KICKOS_HAVE_MPU && defined(KICKOS_ENABLE_SELFTEST)
    TAP_ADD("endpoint_bound", t_endpoint_bound, UNREACHED_RAM);
#endif
    // Console handover mechanism: production syscalls, every board.
    TAP_ADD("cap_gen_reuse", t_cap_gen_reuse);
    TAP_ADD("cap_child_width", t_cap_child_width);
#undef TAP_ADD
// Region 6.
#if KICKOS_SELFTEST_REGION(6)
#define TAP_ADD(name, ...) TAP_ADD_LIVE(name, __VA_ARGS__)
#else
#define TAP_ADD(name, ...) TAP_ADD_ELIDED(name, __VA_ARGS__)
#endif
    TAP_ADD("cap_reply_bound_fast", t_cap_reply_bound_fast);
    TAP_ADD("cap_reply_bound_slow", t_cap_reply_bound_slow);
    TAP_ADD_PINNED("cap_reply_release_close", t_cap_reply_release_close);
    TAP_ADD_PINNED("cap_reply_slot_reuse", t_cap_reply_slot_reuse);
    TAP_ADD("console_publish_priv", t_console_publish);
#if KICKOS_REBOOT
    TAP_ADD("reboot_priv", t_reboot_denied);
#endif
    TAP_ADD("authority_cap", t_authority_cap);
    TAP_ADD("task_authority", t_task_authority, TA_RAM);
    TAP_ADD("periph_enable_unheld", t_periph_enable_unheld, PE_RAM);
    TAP_ADD("periph_reg_write_unheld", t_periph_reg_write_unheld);
#if KICKOS_ARCH_SIM
    TAP_ADD("periph_reg_write_mask", t_periph_reg_write_mask);
    TAP_ADD("window_list", t_window_list);
#endif
    TAP_ADD("privileged_spawn_refused", t_privileged_spawn_refused);
    TAP_ADD_PINNED("thread_join", t_thread_join);
    TAP_ADD("join_stale_gen", t_join_stale_gen);
    TAP_ADD_PINNED("join_timeout", t_join_timeout);
    TAP_ADD_PINNED("task_exit_entry_return", t_task_exit_entry_return);
    TAP_ADD("task_exit_member_exit", t_task_exit_member_exit);
#undef TAP_ADD
// Region 7.
#if KICKOS_SELFTEST_REGION(7)
#define TAP_ADD(name, ...) TAP_ADD_LIVE(name, __VA_ARGS__)
#else
#define TAP_ADD(name, ...) TAP_ADD_ELIDED(name, __VA_ARGS__)
#endif
    TAP_ADD("task_dead_after_sweep", t_task_dead_after_sweep);
    TAP_ADD("task_dead_after_every_sweep", t_task_dead_after_every_sweep);
    TAP_ADD("task_handles", t_task_handles);
    TAP_ADD("task_member_refusals", t_task_member_refusals, TMR_RAM);
    TAP_ADD("task_creator_gate", t_task_creator_gate);
    TAP_ADD_PINNED("task_group_kill", t_task_group_kill);
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
#define TAP_ADD(name, ...) TAP_ADD_LIVE(name, __VA_ARGS__)
#else
#define TAP_ADD(name, ...) TAP_ADD_ELIDED(name, __VA_ARGS__)
#endif
    TAP_ADD_PINNED("arm_hold", t_arm_hold);
#if defined(KICKOS_ENABLE_SELFTEST)
    TAP_ADD_IRQ("irq_thread_ctx", t_irq);
    TAP_ADD_IRQ("irq_as_event", t_irqdrv, IRQDRV_RAM);
    TAP_ADD_IRQ("irq_mask_coalesce", t_irq_mask);
    TAP_ADD_IRQ("irq_discard", t_irq_discard);
    TAP_ADD_IRQ("irq_raise_masked", t_irq_raise_masked);
    TAP_ADD_IRQ("irq_autorearm", t_irq_autorearm);
    TAP_ADD_IRQ("irq_phantom_wake", t_irq_phantom);
    TAP_ADD_IRQ("irq_ownership", t_irq_ownership);
    TAP_ADD_IRQ("irq_stale_register", t_irq_stale_register);
    TAP_ADD_IRQ("irq_claim_gate", t_irq_claim_gate);
    TAP_ADD_IRQ("irq_reclaim", t_irq_reclaim);
    TAP_ADD_IRQ("irq_server_handover", t_irq_server_handover);
#if KICKOS_KERNEL_CORES > 1
    TAP_ADD("irq_cross_core_wake", t_irq_cross_core_wake);
    TAP_ADD("irq_reclaim_stale_raise", t_irq_reclaim_stale_raise);
#endif
#endif
#undef TAP_ADD
// Region 9.
#if KICKOS_SELFTEST_REGION(9)
#define TAP_ADD(name, ...) TAP_ADD_LIVE(name, __VA_ARGS__)
#else
#define TAP_ADD(name, ...) TAP_ADD_ELIDED(name, __VA_ARGS__)
#endif
#if KICKOS_HAVE_ASPACE
    TAP_ADD("caller_stack_arena", t_caller_stack_arena);
#endif
    TAP_ADD("mem_self_grant_nonpow2", t_selfgrant_nonpow2, SGNP_RAM);
#if not KICKOS_HAVE_ASPACE
    TAP_ADD("region_mode", t_region_mode, RM_RAM);
#endif
    TAP_ADD("domain_share", t_domain_share, DOM_RAM);
#if KICKOS_MEMORY_ENFORCED
    // Both backends, and neither a flat board: there is no ownership to breach where
    // nothing is enforced, and every case here would be admitted.
    TAP_ADD("cross_task_block", t_cross_task_block, XG_RAM);
#endif
    TAP_ADD("mmio_grant", t_mmio_grant);
#if KICKOS_FAULT_ISOLATION
    TAP_ADD("task_exit_driver_trap", t_task_exit_driver_trap);
#endif
#if KICKOS_MEMORY_ENFORCED && KICKOS_FAULT_ISOLATION
    TAP_ADD("window_memory_ro", t_window_memory_ro, WRO_RAM);
    TAP_ADD("task_exit_member_fault", t_task_exit_member_fault, TX_RAM);
    TAP_ADD("task_exit_entry_killed", t_task_exit_entry_killed);
    TAP_ADD("task_exit_cancelled_fault", t_task_exit_cancelled_fault, TX_RAM);
    TAP_ADD("task_exit_implicit_fault", t_task_exit_implicit_fault, TX_RAM, TXI_RAM);
#endif
#if defined(KICKOS_ENABLE_SELFTEST) && (KICKOS_HAVE_MPU || defined(KICKOS_SELFTEST_SPARE_DEV))
    TAP_ADD("window_get", t_window_get, WG_RAM);
#endif
#if defined(KICKOS_ENABLE_SELFTEST) && not KICKOS_HAVE_ASPACE
    TAP_ADD_PINNED("uncached_grant_sync", t_uncached_grant_sync, UG_RAM);
#endif
    TAP_ADD("caller_stack", t_caller_stack, CSTK_RAM);
#if not KICKOS_HAVE_ASPACE
    TAP_ADD("caller_stack_overlap", t_caller_stack_overlap, CSTK_RAM);
#endif
#undef TAP_ADD
// Region 10.
#if KICKOS_SELFTEST_REGION(10)
#define TAP_ADD(name, ...) TAP_ADD_LIVE(name, __VA_ARGS__)
#else
#define TAP_ADD(name, ...) TAP_ADD_ELIDED(name, __VA_ARGS__)
#endif
#if KICKOS_HAVE_MPU
    TAP_ADD("stackbase_arena", t_stackbase_arena);
#if defined(KICKOS_ENABLE_SELFTEST)
    TAP_ADD("grant_reserved", t_grant_reserved);
    TAP_ADD("dev_window_exclusive", t_dev_window_exclusive);
#endif
#endif
#if KICKOS_HAVE_ASPACE && defined(KICKOS_ENABLE_SELFTEST)
    TAP_ADD("cap_map", t_cap_map);
    TAP_ADD("cap_map_over_stack", t_cap_map_over_stack);
    TAP_ADD("stack_grant_refused", t_stack_grant_refused);
    TAP_ADD("stack_handoff_refused", t_stack_handoff_refused);
    TAP_ADD("stack_slot_returns", t_stack_slot_returns);
    TAP_ADD("cap_map_pins_run", t_cap_map_pins_run);
    TAP_ADD("cap_share", t_cap_share);
    TAP_ADD("frame_mint_census", t_frame_mint_census);
    TAP_ADD("space_cap_dies_with_task", t_space_cap_dies_with_task);
    TAP_ADD("aspace_seam", t_aspace_seam, SEAM_RAM);
    TAP_ADD("aspace_churn", t_aspace_churn);
    TAP_ADD("stack_is_frames", t_stack_is_frames);
    TAP_ADD("aspace_two_spaces_same_grant", t_aspace_two_spaces_same_grant, SP_RAM);
    TAP_ADD("process_private_data", t_process_private_data, PW_RAM);
    TAP_ADD("task_siblings_share", t_task_siblings_share, SIB_RAM);
    TAP_ADD("task_handoff_readback", t_task_handoff_readback, HO_RAM);
    TAP_ADD("task_handoff_donor_exits", t_task_handoff_donor_exits, DX_RAM);
    TAP_ADD("task_handoff_slice", t_task_handoff_slice, SL_RAM);
    TAP_ADD("reservation_teardown", t_reservation_teardown);
    TAP_ADD("frame_scrub_cross_task", t_frame_scrub_cross_task);
    TAP_ADD("spawn_refusal_frees_task", t_spawn_refusal_frees_task);
    TAP_ADD("spawn_refusal_frees_donor", t_spawn_refusal_frees_donor);
    TAP_ADD("parked_frame_hostile", t_parked_frame_hostile, PFH_RAM);
    TAP_ADD("process_ipc_same_addr", t_process_ipc_same_addr, PI_RAM);
    TAP_ADD("process_call_reply", t_process_call_reply, PI_RAM);
    TAP_ADD("grant_kernel_word_refused", t_grant_kernel_word_refused, KW_RAM);
    TAP_ADD("uncached_alias_sync", t_uncached_alias_sync, UA_RAM);
    TAP_ADD("app_pointers_relocated", t_app_pointers_relocated);
    TAP_ADD("recv_buf_unmapped", t_recv_buf_unmapped);
    TAP_ADD("frame_run_slot_recycle", t_frame_run_slot_recycle);
    TAP_ADD("call_reply_undisclosed", t_call_reply_undisclosed);
    TAP_ADD("process_data_from_image", t_process_data_from_image, FI_RAM);
#endif
#if defined(KICKOS_ENABLE_SELFTEST)
    TAP_ADD_IRQ("irq_kernel_line_reserved", t_irq_kernel_line_reserved);
    TAP_ADD("prio_ceiling_refused", t_prio_ceiling_refused);
    TAP_ADD("prio_ceiling_narrow_only", t_prio_ceiling_narrow_only);
#endif
#if defined(KICKOS_ENABLE_SELFTEST) && KICKOS_KERNEL_CORES > 1
    TAP_ADD("pin_places", t_pin_places);
    TAP_ADD("unpin_restores", t_unpin_restores);
    TAP_ADD("pin_beyond_grant_refused", t_pin_beyond_grant_refused);
    TAP_ADD("affinity_undriven_refused", t_affinity_undriven_refused);
    TAP_ADD("migrate_running", t_migrate_running);
    TAP_ADD("resched_reaches_pinned_caller", t_resched_reaches_pinned_caller);
    TAP_ADD("prio_self_lower_moves_waiter", t_prio_self_lower_moves_waiter);
    TAP_ADD("pin_cross_task_refused", t_pin_cross_task_refused);
    TAP_ADD("grant_narrows", t_grant_narrows);
    TAP_ADD("grant_after_member_refused", t_grant_after_member_refused);
    TAP_ADD("grant_second_narrow_only", t_grant_second_narrow_only);
    TAP_ADD("grant_inherited_by_child_task", t_grant_inherited_by_child_task);
    TAP_ADD("isolated_single_grant_ok", t_isolated_single_grant_ok);
    TAP_ADD("affinity_dead_handle_refused", t_affinity_dead_handle_refused);
    TAP_ADD("isolated_unpinned_never", t_isolated_unpinned_never);
    TAP_ADD("isolated_unpin_excludes", t_isolated_unpin_excludes);
    TAP_ADD("isolated_takes_pinned", t_isolated_takes_pinned);
    TAP_ADD("isolated_mixed_mask_ok", t_isolated_mixed_mask_ok);
    TAP_ADD("slice_preempts_every_core", t_slice_preempts_every_core);
    TAP_ADD("threads_reach_every_core", t_threads_reach_every_core);
    TAP_ADD("reent_per_thread_cores", t_reent_per_thread_cores);
#if defined(__x86_64__)
    TAP_ADD("fp_enabled_every_core", t_fp_enabled_every_core);
#endif
#endif
#if KICKOS_HAVE_ASPACE && defined(KICKOS_ENABLE_SELFTEST) && KICKOS_FAULT_ISOLATION
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
    TAP_ADD("amp_count_reads", t_amp_count_reads);
    TAP_ADD("amp_port_seating", t_amp_port_seating);
    TAP_ADD("amp_port_unnamed", t_amp_port_unnamed);
#if KICKOS_AMP_OWN_IMAGE && KICKOS_MEMORY_ENFORCED && KICKOS_AMP_USER_SHARE_SIZE != 0
    TAP_ADD("amp_share_window", t_amp_share_window, ASW_RAM);
#endif
    // Each needs a peer that is running, and skips by name where none answers.
    TAP_ADD("amp_far_call", t_amp_far_call);
    TAP_ADD("amp_far_reply_empty", t_amp_far_reply_empty);
#if KICKOS_AMP_OWN_IMAGE
    TAP_ADD("amp_share_crossing", t_amp_share_crossing);
#if KICKOS_HAVE_ASPACE
    TAP_ADD("amp_far_reply_unmapped", t_amp_far_reply_unmapped);
#endif
#endif
    TAP_ADD("amp_crossing_task_local", t_amp_crossing_task_local, AG_RAM);
    // Last of the block: it fills the endpoint pool, so an arm run while it holds the slots
    // would be refused one, and it closes the port every receiving arm needs.
    TAP_ADD("amp_local_port_slot_held", t_amp_local_port_slot_held);
#endif
    TAP_ADD("ipc_one_buffer_both_ends", t_ipc_one_buffer_both_ends);
    TAP_ADD("confused_deputy", t_confused_deputy, CD_RAM);
    // After every arm that reads the console's state: these two publish it and leave it
    // reclaimed, polled for every line after.
    TAP_ADD_PINNED("console_publish_handout", t_console_publish_handout);
    TAP_ADD("console_publish_narrow", t_console_publish_narrow);
    TAP_ADD("mem_self_grant", t_selfgrant, SG_ASK);
#undef TAP_ADD
// A region this file cuts but the build does not know about is elided from EVERY image and
// runs nowhere, which no plan check can see.
#if KICKOS_SELFTEST_REGIONS != 10
#error "this file cuts the registry into ten regions; say so in its CMakeLists"
#endif

    if (not st_ram_seat())
    {
        exit(1);
    }
    // The failure count is the system's exit status.
    exit(tap::run_all());
}
