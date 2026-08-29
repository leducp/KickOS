// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// SCAFFOLDING: the arms that witness the console, the clock, the one-shot timer, the software
// interrupt controller, the context switch and idle, taken AT THE ARCH SEAM.

#include <kickos/arch/apic.h>
#include <kickos/arch/arch.h>
#include <kickos/chip_q35.h>

#include <stddef.h>
#include <stdint.h>

// tools/run-qemu-x86_64-x3.sh carries this string too; move both or the arm fails.
#define KICKOS_X3_TOKEN "KICKOS-X3 e73b1f04 x86_64/q35 seam"

namespace
{
    constexpr uint64_t ns_per_ms = 1000000;

    // The burn is twice the slice, so a worker is preempted mid-burn and logs again only after
    // being scheduled back in. The log is written by the WORKERS, never by the interrupt.
    constexpr uint64_t rr_slice_ns = 5 * ns_per_ms;
    constexpr uint64_t rr_burn_ns = 2 * rr_slice_ns;
    constexpr uint32_t rr_slices = 14;

    constexpr uint64_t timer_deadline_ns = 20 * ns_per_ms;
    // A deadline may not be honoured EARLY, and TCG's interrupt latency is what the upper
    // bound has to leave room for.
    constexpr uint64_t timer_late_bound_ns = 20 * ns_per_ms;

    constexpr int irq_line = 7;
    // Distinct from irq_line, and inside KICKOS_MAX_IRQ.
    constexpr int irq_line_b = 3;

    // A spin bound in clock nanoseconds, so an arm that never completes fails and does not hang.
    constexpr uint64_t spin_bound_ns = 2000 * ns_per_ms;

    enum phase
    {
        phase_idle,
        phase_timer,
        phase_rr,
    };

    volatile phase g_phase = phase_idle;

    volatile uint32_t g_timer_count = 0;
    volatile uint64_t g_timer_at = 0;

    volatile uint32_t g_irq_count = 0;
    // Bit set = that line reached kickos_isr_irq. A count alone cannot tell two deliveries of
    // one line from one delivery each of two.
    volatile uint32_t g_irq_seen = 0;
    volatile int g_irq_last_line = -1;
    volatile int g_irq_in_isr = -1;

    volatile uint32_t g_deferred = 0;
    volatile uint32_t g_rr_seen = 0;
    // One interrupt rescheduling MORE THAN ONCE, the case g_ctx_current exists for.
    volatile uint32_t g_double_request = 0;
    constexpr uint32_t rr_double_at = 3;

    char g_log[32];
    volatile uint32_t g_log_n = 0;

    bool g_failed = false;

    struct arch_context g_main;
    struct arch_context g_vol;
    struct arch_context g_worker_a;
    struct arch_context g_worker_b;
    struct arch_context* g_rr_running = nullptr;

    alignas(16) uint8_t g_stack_vol[16384];
    alignas(16) uint8_t g_stack_a[16384];
    alignas(16) uint8_t g_stack_b[16384];

    void put(char const* s)
    {
        size_t n = 0;
        while (s[n] != '\0')
        {
            n++;
        }
        arch_console_write(s, n);
    }

    void put_dec(uint64_t v)
    {
        char buf[21];
        int n = 0;
        if (v == 0)
        {
            arch_console_write("0", 1);
            return;
        }
        while (v != 0)
        {
            buf[n] = static_cast<char>('0' + (v % 10));
            n++;
            v /= 10;
        }
        while (n > 0)
        {
            n--;
            arch_console_write(&buf[n], 1);
        }
    }

    void log_letter(char c)
    {
        uint32_t const n = g_log_n;
        if (n + 1 < sizeof(g_log))
        {
            g_log[n] = c;
            g_log[n + 1] = '\0';
            g_log_n = n + 1;
        }
    }

    void arm(char const* name, bool ok)
    {
        put("  ");
        put(KICKOS_X3_TOKEN);
        put(" arm=");
        put(name);
        put(" ok=");
        put_dec(static_cast<uint64_t>(ok));
        put("\n");
        if (not ok)
        {
            g_failed = true;
        }
    }

    // Spins with interrupts LIVE until the predicate holds or the clock runs past the bound.
    bool wait_for_count(volatile uint32_t const* counter, uint32_t target)
    {
        uint64_t const deadline = arch_clock_now() + spin_bound_ns;
        while (*counter < target)
        {
            if (arch_clock_now() > deadline)
            {
                return false;
            }
        }
        return true;
    }

    void burn_ns(uint64_t ns)
    {
        uint64_t const until = arch_clock_now() + ns;
        while (arch_clock_now() < until)
        {
        }
    }

    void worker_a(void*)
    {
        while (true)
        {
            log_letter('A');
            burn_ns(rr_burn_ns);
        }
    }

    void worker_b(void*)
    {
        while (true)
        {
            log_letter('B');
            burn_ns(rr_burn_ns);
        }
    }

    // The voluntary-switch worker: it logs, hands control back, logs again when it is switched
    // back in, and then RETURNS, so the thread-exit trampoline switch.S seats is exercised too.
    void worker_voluntary(void*)
    {
        log_letter('V');
        arch_switch(&g_vol, &g_main);
        log_letter('V');
    }

    // --- The arms ---------------------------------------------------------------
    void arm_report(void)
    {
        put("  ");
        put(KICKOS_X3_TOKEN);
        put(" apic mode=");
        if (kickos::x86_64::apic_is_x2())
        {
            put("x2apic");
        }
        else
        {
            put("xapic");
        }
        put(" timer_hz=");
        put_dec(kickos::x86_64::apic_timer_hz());
        put(" tsc_hz=");
        put_dec(kickos::x86_64::apic_tsc_hz());
        put(" ref_hz=");
        put_dec(kickos_x86_ref_hz());
        put(" ram_size=");
        put_dec(arch_ram_size());
        put("\n");
        arm("calibrated", kickos::x86_64::apic_timer_hz() != 0
                              and kickos::x86_64::apic_tsc_hz() != 0);
    }

    // The clock never goes backwards across an arm and a disarm.
    void arm_clock(void)
    {
        uint64_t const t0 = arch_clock_now();
        arch_timer_disarm();
        uint64_t const t1 = arch_clock_now();
        arch_timer_arm(t0 + spin_bound_ns);
        uint64_t const t2 = arch_clock_now();
        arch_timer_disarm();
        uint64_t const t3 = arch_clock_now();

        put("  ");
        put(KICKOS_X3_TOKEN);
        put(" clock t0=");
        put_dec(t0);
        put(" t3=");
        put_dec(t3);
        put("\n");
        arm("clock_monotonic", t1 >= t0 and t2 >= t1 and t3 >= t2);
        arm("clock_advances", t3 > t0);
    }

    // The deadline is honoured, not early, and inside the tolerance.
    void arm_timer(void)
    {
        g_phase = phase_timer;
        g_timer_count = 0;
        uint64_t const deadline = arch_clock_now() + timer_deadline_ns;
        arch_timer_arm(deadline);
        __asm__ volatile("sti" ::: "memory");
        bool const fired = wait_for_count(&g_timer_count, 1);
        uint64_t const at = g_timer_at;
        arch_timer_disarm();

        put("  ");
        put(KICKOS_X3_TOKEN);
        put(" timer want_ns=");
        put_dec(timer_deadline_ns);
        put(" fired=");
        put_dec(static_cast<uint64_t>(fired));
        if (fired and at >= deadline)
        {
            put(" late_ns=");
            put_dec(at - deadline);
        }
        put("\n");
        arm("timer_fired", fired);
        arm("timer_not_early", fired and at >= deadline);
        arm("timer_in_tolerance", fired and at >= deadline
                                     and (at - deadline) < timer_late_bound_ns);

        // The disarm above must mean no callback fires. Nothing is latched after an expiry the
        // handler already took, so a further count here is a disarm that did not disarm.
        uint32_t const after = g_timer_count;
        burn_ns(4 * timer_deadline_ns);
        arm("timer_disarmed", g_timer_count == after);
        __asm__ volatile("cli" ::: "memory");
        g_phase = phase_idle;
    }

    void arm_irq(void)
    {
        g_irq_count = 0;
        g_irq_last_line = -1;
        g_irq_in_isr = -1;
        arch_irq_clear_pending(irq_line);
        arch_irq_unmask(irq_line);
        __asm__ volatile("sti" ::: "memory");
        arch_irq_inject(irq_line);
        bool const delivered = wait_for_count(&g_irq_count, 1);

        put("  ");
        put(KICKOS_X3_TOKEN);
        put(" irq line=");
        put_dec(irq_line);
        put(" delivered=");
        put_dec(static_cast<uint64_t>(delivered));
        put(" got_line=");
        put_dec(static_cast<uint64_t>(g_irq_last_line + 1));
        put(" in_isr=");
        put_dec(static_cast<uint64_t>(g_irq_in_isr));
        put("\n");
        arm("irq_delivered", delivered);
        arm("irq_line_identity", g_irq_last_line == irq_line);
        arm("irq_in_isr", g_irq_in_isr == 1);

        // The mask side, and the one-deep latch with it: three raises on a masked line must
        // deliver nothing, and the unmask must then redeliver exactly one.
        arch_irq_mask(irq_line);
        uint32_t const before = g_irq_count;
        arch_irq_inject(irq_line);
        arch_irq_inject(irq_line);
        arch_irq_inject(irq_line);
        burn_ns(10 * ns_per_ms);
        bool const silent = g_irq_count == before;
        arch_irq_unmask(irq_line);
        burn_ns(10 * ns_per_ms);
        uint32_t const coalesced = g_irq_count - before;

        put("  ");
        put(KICKOS_X3_TOKEN);
        put(" irq masked_delivered=");
        put_dec(static_cast<uint64_t>(g_irq_count - before));
        put(" coalesced=");
        put_dec(coalesced);
        put("\n");
        arm("irq_mask_silences", silent);
        arm("irq_latch_one_deep", coalesced == 1);

        // Two different lines rung inside ONE interrupts-masked region. The local APIC
        // coalesces the two self-directed raises into one delivery, so what decides whether
        // both lines arrive is what the doorbell carries.
        arch_irq_clear_pending(irq_line);
        arch_irq_clear_pending(irq_line_b);
        arch_irq_unmask(irq_line);
        arch_irq_unmask(irq_line_b);
        g_irq_count = 0;
        g_irq_seen = 0;
        arch_irq_state_t const held = arch_irq_save();
        arch_irq_inject(irq_line);
        arch_irq_inject(irq_line_b);
        arch_irq_restore(held);
        bool const both = wait_for_count(&g_irq_count, 2);
        uint32_t const seen = g_irq_seen;
        uint32_t const want = (1u << irq_line) | (1u << irq_line_b);

        put("  ");
        put(KICKOS_X3_TOKEN);
        put(" irq two_lines delivered=");
        put_dec(g_irq_count);
        put(" seen=");
        put_dec(seen);
        put(" want=");
        put_dec(want);
        put("\n");
        arm("irq_two_lines_one_region", both and seen == want);

        arch_irq_mask(irq_line_b);
        arch_irq_mask(irq_line);
        __asm__ volatile("cli" ::: "memory");
    }

    void arm_switch_voluntary(void)
    {
        g_log_n = 0;
        g_log[0] = '\0';
        log_letter('M');
        arch_context_init(&g_vol, worker_voluntary, nullptr, g_stack_vol,
                          sizeof(g_stack_vol), 1);
        arch_switch(&g_main, &g_vol);
        // A second switch in resumes the worker past its own hand-back, and its entry then
        // RETURNS, so the thread-exit trampoline switch.S seats is on this path too.
        arch_switch(&g_main, &g_vol);
        log_letter('M');

        put("  ");
        put(KICKOS_X3_TOKEN);
        put(" switch voluntary order=");
        put(g_log);
        put("\n");
        arm("switch_voluntary", g_log_n == 5 and g_log[0] == 'M' and g_log[1] == 'V'
                                   and g_log[2] == 'V' and g_log[3] == 'X'
                                   and g_log[4] == 'M');
    }

    // Two threads round-robin in a deterministic order. The switch is DEFERRED here, taken at
    // the interrupt exit, so this is the arm that witnesses that path.
    void arm_switch_preempt(void)
    {
        g_log_n = 0;
        g_log[0] = '\0';
        g_deferred = 0;
        g_rr_seen = 0;
        g_double_request = 0;
        arch_context_init(&g_worker_a, worker_a, nullptr, g_stack_a, sizeof(g_stack_a), 1);
        arch_context_init(&g_worker_b, worker_b, nullptr, g_stack_b, sizeof(g_stack_b), 1);
        g_rr_running = &g_worker_a;
        g_phase = phase_rr;
        arch_timer_arm(arch_clock_now() + rr_slice_ns);
        // Enters worker A; the interrupt exit is what alternates them, and the last slice
        // switches back here.
        arch_switch(&g_main, &g_worker_a);
        g_phase = phase_idle;
        arch_timer_disarm();
        __asm__ volatile("cli" ::: "memory");

        uint32_t a_count = 0;
        uint32_t b_count = 0;
        bool alternating = true;
        for (uint32_t i = 0; i < g_log_n; i++)
        {
            if (g_log[i] == 'A')
            {
                a_count++;
            }
            if (g_log[i] == 'B')
            {
                b_count++;
            }
            if (i > 0 and g_log[i] == g_log[i - 1])
            {
                alternating = false;
            }
        }

        put("  ");
        put(KICKOS_X3_TOKEN);
        put(" switch preempt order=");
        put(g_log);
        put(" deferred=");
        put_dec(g_deferred);
        put("\n");
        arm("switch_preempt_both_ran", a_count >= 3 and b_count >= 3);
        arm("switch_preempt_alternates", alternating);
        arm("switch_preempt_deferred", g_deferred >= 6);
        arm("switch_preempt_double_request", g_double_request >= 1);
    }

    // arch_idle_wait must return with interrupts MASKED on entry, which a bare HLT cannot do.
    void arm_idle(void)
    {
        g_phase = phase_timer;
        g_timer_count = 0;
        arch_timer_arm(arch_clock_now() + 10 * ns_per_ms);
        arch_irq_state_t const state = arch_irq_save();
        arch_idle_wait();
        arch_irq_restore(state);
        bool const masked_woke = g_timer_count > 0;
        arch_timer_disarm();

        uint32_t const before = g_timer_count;
        arch_timer_arm(arch_clock_now() + 10 * ns_per_ms);
        __asm__ volatile("sti" ::: "memory");
        arch_idle_wait();
        bool const open_woke = g_timer_count > before;
        arch_timer_disarm();
        __asm__ volatile("cli" ::: "memory");
        g_phase = phase_idle;

        arm("idle_wakes_masked", masked_woke);
        arm("idle_wakes_open", open_woke);
    }

    void arm_flush(void)
    {
        arch_console_flush_sync();
        arm("flush_sync_returns", true);
    }
}

extern "C"
{

void kickos_isr_timer(void)
{
    g_timer_count = g_timer_count + 1;
    g_timer_at = arch_clock_now();
    if (g_phase != phase_rr)
    {
        return;
    }
    g_rr_seen = g_rr_seen + 1;
    if (g_rr_seen >= rr_slices)
    {
        arch_switch(g_rr_running, &g_main);
        g_deferred = g_deferred + 1;
        return;
    }
    struct arch_context* next = &g_worker_a;
    if (g_rr_running == &g_worker_a)
    {
        next = &g_worker_b;
    }
    struct arch_context* const from = g_rr_running;
    g_rr_running = next;
    if (g_rr_seen == rr_double_at)
    {
        // A request to stay put, superseded one line later. Only the LAST target may run: if
        // the first won, this worker would take a second slice and the log would repeat a
        // letter, which switch_preempt_alternates is what catches.
        arch_switch(from, from);
        g_double_request = g_double_request + 1;
    }
    arch_switch(from, next);
    g_deferred = g_deferred + 1;
    arch_timer_arm(arch_clock_now() + rr_slice_ns);
}

void kickos_isr_irq(int irq)
{
    g_irq_count = g_irq_count + 1;
    g_irq_seen = g_irq_seen | (1u << (static_cast<unsigned>(irq) & 31u));
    g_irq_last_line = irq;
    g_irq_in_isr = arch_in_isr();
}

// switch.S routes a thread whose entry returned here.
void kickos_thread_return(void)
{
    log_letter('X');
    arch_switch(&g_vol, &g_main);
    while (true)
    {
        __asm__ volatile("cli\n\thlt");
    }
}

void kickos_x86_64_landed(uintptr_t ram_base, uint64_t ram_size)
{
    kickos::q35::ram_publish(ram_base, static_cast<size_t>(ram_size));
    arch_init();

    put("\n" KICKOS_X3_TOKEN " arms\n");
    arm_report();
    arm_clock();
    arm_timer();
    arm_irq();
    arm_switch_voluntary();
    arm_switch_preempt();
    arm_idle();
    arm_flush();

    if (g_failed)
    {
        put(KICKOS_X3_TOKEN " FAIL\n");
        arch_shutdown(1);
    }
    put(KICKOS_X3_TOKEN " PASS\n");
    arch_shutdown(0);
}

}
