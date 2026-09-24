// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Architecture and runtime hooks for kernel fixtures. To check required symbols:
//   nm --undefined-only <objects> | comm -23 - <defined-symbols>
// Build with -fno-exceptions to avoid unwinding dependencies.
// kpanic terminates the process. Use KICKOS_EXPECT_PANIC so gtest captures
// the panic output and matches tests/lib/panic.ere.

#include <stdio.h>
#include <stdlib.h>

#include <kickos/irq_route.h>
#include <kickos/console_tx.h>
#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
#include <kickos/kernel.h>
#include <kickos/klock.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/time.h>

#include "kfixture.h"

extern "C"
{
    // A zero lock count marks a capability-sweep gap for injected actions.
    arch_irq_state_t arch_irq_save(void)
    {
        kickos::testfix::note_irq_save();
        return 0;
    }

    void arch_irq_restore(arch_irq_state_t)
    {
        kickos::testfix::note_irq_restore();
    }

    int arch_in_isr(void)
    {
        if (kickos::testfix::g_in_isr)
        {
            return 1;
        }
        return 0;
    }

    void arch_mpu_apply(struct arch_mpu_region const*, size_t, struct arch_mpu_encoded const*)
    {
    }

    // This fixture describes MPU regions but does not enforce them.
    uint32_t arch_mpu_encode(struct arch_mpu_region const*, size_t n, struct arch_mpu_encoded*)
    {
        return (static_cast<uint32_t>(1) << n) - 1u;
    }

    // IRQ tests inspect Kernel::irq_table; the default handler marks a free line.
    void arch_irq_mask(int line)
    {
        if (line >= 0 and line < KICKOS_MAX_IRQ)
        {
            kickos::testfix::g_line_armed[line] = false;
        }
    }

    void arch_irq_unmask(int line)
    {
        if (line >= 0 and line < KICKOS_MAX_IRQ)
        {
            kickos::testfix::g_line_armed[line] = true;
        }
    }

    void arch_irq_route(int line, uint32_t)
    {
        kickos::testfix::g_routed_armed = 0;
        if (line >= 0 and line < KICKOS_MAX_IRQ and kickos::testfix::g_line_armed[line])
        {
            kickos::testfix::g_routed_armed = 1;
        }
    }

    bool arch_irq_line_kernel_owned(int)
    {
        return false;
    }

    void arch_irq_clear_pending(int line)
    {
        if (line >= 0 and line < KICKOS_MAX_IRQ)
        {
            kickos::testfix::g_line_clears[line]++;
        }
    }

    void arch_idle_wait(void)
    {
        kickos::testfix::note_park();
    }

#if KICKOS_NUM_CORES > 1
    // At one core arch_cpu_id is a macro folding to a literal and no source in the tree may
    // define it.
    uint32_t arch_cpu_id(void)
    {
        return kickos::testfix::g_core;
    }
#endif

#if KICKOS_KERNEL_CORES > 1
    void arch_kernel_lock(void)
    {
        kickos::testfix::note_klock_acquire();
    }

    void arch_kernel_unlock(void)
    {
        kickos::testfix::note_klock_release();
    }

#if KICKOS_DEBUG
    int arch_kernel_lock_held(void)
    {
        if (kickos::testfix::klock_held())
        {
            return 1;
        }
        return 0;
    }
#endif

    void arch_ipi_resched_self(void)
    {
        kickos::testfix::g_ipi_self_raises++;
    }

    void arch_ipi_send(uint32_t cores)
    {
        kickos::testfix::g_ipi_sends++;
        kickos::testfix::g_ipi_send_mask |= cores;
    }

    void arch_ipi_wait(uint32_t)
    {
    }
#endif

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
    uint32_t arch_trace_now(void)
    {
        return static_cast<uint32_t>(kickos::testfix::g_now_ns);
    }

    int kickos_rtt_write_record_ch1(uint8_t const*, size_t)
    {
        return 1; // accepted: a 0 here would count a drop on every record
    }
#endif

    // The stub returns, so restore the lock and depth that sched::start
    // will release when its scope exits.
    void arch_start(struct arch_context*, struct arch_context*)
    {
        kickos::klock_enter();
    }

    // Record a switch without changing machine context.
    void arch_switch(struct arch_context* from, struct arch_context* to)
    {
        kickos::testfix::note_switch(kickos::testfix::thread_of_context(from),
                                     kickos::testfix::thread_of_context(to));
    }

    // Record context rebuilds without performing them.
    void arch_ctx_redirect(struct arch_context* ctx, void (*entry)(void* arg),
                           void* stack_base, size_t stack_size)
    {
        kickos::testfix::note_ctx_redirect(kickos::testfix::thread_of_context(ctx), entry,
                                           stack_base, stack_size);
    }

    // Link-only stubs.
    bool arch_fault_is_user_thread(void*)
    {
        return false;
    }

    void arch_fault_redirect_to_exit(void*)
    {
    }

    // Trace reclamation after the full sweep to check its order relative to switches.
    void console_note_driver_death(void)
    {
        kickos::testfix::g_console_noted++;
        kickos::testfix::trace_add("note");
    }

    void console_on_driver_death(void)
    {
        kickos::testfix::g_console_reclaimed++;
        kickos::testfix::trace_add("reclaim");
    }

    // Always fail shutdown so exit(0) cannot hide unrun tests.
    // Tests of last-thread exit must keep another thread live.
    void kickos_terminate(int status)
    {
        printf("FIXTURE FAIL: kickos_terminate(%d) ended the arm\n", status);
        exit(1);
    }
}

namespace kickos
{
    void kpanic(char const* msg)
    {
        printf("KERNEL PANIC: %s\n", msg);
        fflush(stdout);
        exit(1);
    }

#if KICKOS_DIAG_TERSE
    void kpanic_at(char const* file, unsigned line)
    {
        printf("KERNEL PANIC: %s:%u\n", file, line);
        fflush(stdout);
        exit(1);
    }
#endif

    // Link-only stub.
    void kprintf_fault(char const*, ...)
    {
    }

    // Use a distinct domain per task so reference counts can be tested independently.
    // The production default-user singleton is not shared in this fixture.
    Domain* domain_for(uint32_t, void*, size_t, uint32_t, Domain*, int* err)
    {
        *err = 0;
        for (int i = 0; i < testfix::FIXTURE_DOMAIN_SLOTS; i++)
        {
            if (testfix::g_domain_refs[i] == 0 and not testfix::g_domain_live[i])
            {
                testfix::g_domain_live[i] = true;
                return &testfix::g_domains[i];
            }
        }
        printf("FIXTURE FAIL: fake domain pool exhausted\n");
        exit(1);
    }

    void domain_ref(Domain* d)
    {
        int const i = testfix::domain_index(d);
        if (i < 0)
        {
            return;
        }
        testfix::g_domain_refs[i]++;
    }

    void domain_release(Domain* d)
    {
        int const i = testfix::domain_index(d);
        if (i < 0)
        {
            return;
        }
        if (testfix::g_domain_refs[i] == 0)
        {
            // Admission can be abandoned before taking a reference. Free that live slot
            // without decrementing; releasing an already-free slot is an error.
            if (not testfix::g_domain_live[i])
            {
                printf("FIXTURE FAIL: domain_release below zero\n");
                exit(1);
            }
            testfix::g_domain_live[i] = false;
            return;
        }
        testfix::g_domain_refs[i]--;
        if (testfix::g_domain_refs[i] == 0)
        {
            testfix::g_domain_live[i] = false;
        }
#if KICKOS_HAVE_ASPACE
        // Trace when task_release drops the thread's domain reference.
        testfix::note_member_release();
#endif
    }

#ifndef KFIXTURE_REAL_TIME
    uint64_t ktime_now()
    {
        return testfix::g_now_ns;
    }

    void ktime_rearm(Thread const*)
    {
    }

    void ktime_deadline_cancel(Thread* t)
    {
        t->on_timer = false;
    }

    // No clock runs here. Tests trigger expiry with thread_abort_park.
    void ktime_deadline_arm(Thread* t, uint32_t)
    {
        t->on_timer = true;
    }
#endif
}

#ifdef KFIXTURE_REAL_TIME
// The real kernel/time/time.cc answers the ktime half; this is the clock beneath it.
extern "C"
{
    uint64_t arch_clock_now(void)
    {
        return kickos::testfix::g_now_ns;
    }

    void arch_timer_arm(uint64_t)
    {
    }

    void arch_timer_disarm(void)
    {
    }
}
#endif

namespace kickos
{
    // All IRQ lines are local on this single-core fixture.
    void irq_line_op(int line, LineOp op)
    {
        switch (op)
        {
            case LineOp::MASK:
            {
                arch_irq_mask(line);
                break;
            }
            case LineOp::UNMASK:
            {
                arch_irq_unmask(line);
                break;
            }
            case LineOp::CLEAR:
            {
                arch_irq_clear_pending(line);
                break;
            }
        }
    }

    void irq_line_op_local(int line, LineOp op)
    {
        irq_line_op(line, op);
    }
}
