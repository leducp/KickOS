// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The WHOLE seam between the kernel sources a K-seam gate compiles and the rest of the
// image: every symbol here is one those sources leave undefined.
//
// RE-DERIVE THE SET, which is a property of the chosen sources AND of the preset, both of
// which move:
//
//   nm --undefined-only <the objects> | comm -23 - <their defined symbols>
//
// which also reports __gxx_personality_v0 and _Unwind_Resume unless the gate is built
// -fno-exceptions as the CMakeLists does it.
//
// kpanic ends the PROCESS with a message matching tests/lib/panic.ere, so a kernel invariant
// enforced by KICKOS_ASSERT rather than a return code is gated by a gtest death test.
// KICKOS_EXPECT_PANIC in kseam_test.h is the only way to write one: it folds this stdout onto
// the forked child's stderr, which is the stream gtest matches.

#include <stdio.h>
#include <stdlib.h>

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
    // Counted: the moment the count reaches zero inside a capability sweep IS the chunk gap,
    // where run_in_chunk_gap seats an interleaving.
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

    // The interrupt controller. What a gate reads is the DISPATCH TABLE irq.cc keeps in
    // Kernel: a line is free iff it holds the null-object default.
    void arch_irq_mask(int)
    {
    }

    void arch_irq_unmask(int)
    {
    }

    void arch_irq_clear_pending(int)
    {
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

    // The raise a release restores an owed reschedule with.
    void arch_ipi_resched_self(void)
    {
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
    // ktrace.h is header-inline and reaches BOTH of these from kernel/irq/irq.cc.
    uint32_t arch_trace_now(void)
    {
        return static_cast<uint32_t>(kickos::testfix::g_now_ns);
    }

    int kickos_rtt_write_record_ch1(uint8_t const*, size_t)
    {
        return 1; // accepted: a 0 here would count a drop on every record
    }
#endif

    // RETURNS, where the real one does not, into a sched::start bracket that still owes a
    // leave to the lock and the depth. Re-taking both is what lets that bracket unwind: short
    // of it the depth wraps and every later acquire in the process is silently skipped.
    void arch_start(struct arch_context*, struct arch_context*)
    {
        kickos::klock_enter();
    }

    // Returns without switching; see kfixture.h for what that costs an arm.
    void arch_switch(struct arch_context* from, struct arch_context* to)
    {
        kickos::testfix::note_switch(kickos::testfix::thread_of_context(from),
                                     kickos::testfix::thread_of_context(to));
    }

    // Records rather than rebuilds; kfixture.h has what an arm reads.
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

    // Traced as well as counted: the reclaim runs after the whole sweep, so its position in
    // the trace is what dates a switch relative to the sweep.
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

    // LOUD, and never with the status it was handed: on the last-thread-out path an exit(0)
    // would end the gate green with failures already printed and later arms never run. An
    // arm that wants that path keeps a spare thread live.
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

    // The domain pool: store the pointer, count references, free at zero, which is what
    // task.cc asks of it.
    //
    // NO DEDUP: every call hands back a distinct domain, where the real domain_for returns the
    // shared default-user singleton for a no-grant unprivileged task, so an arm reads one
    // task's reference count clear of another task's holds.
    Domain* domain_for(uint32_t, void*, size_t, uint32_t, Domain*, int* err)
    {
        *err = 0;
        for (int i = 0; i < KICKOS_MAX_TASKS; i++)
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
            printf("FIXTURE FAIL: domain_release below zero\n");
            exit(1);
        }
        testfix::g_domain_refs[i]--;
        if (testfix::g_domain_refs[i] == 0)
        {
            testfix::g_domain_live[i] = false;
        }
    }

    uint64_t ktime_now()
    {
        return testfix::g_now_ns;
    }

    void ktime_rearm()
    {
    }

    void ktime_deadline_cancel(Thread* t)
    {
        t->on_timer = false;
    }
}
