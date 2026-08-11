// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The WHOLE seam between the kernel sources a K-seam gate compiles and the rest of the
// image: every symbol here is one those sources name and none of them define. Sixteen,
// re-derived on this tree with
//
//   nm --undefined-only <the five objects> | comm -23 - <their defined symbols>
//
// which also reports __gxx_personality_v0 and _Unwind_Resume unless the gate is built
// -fno-exceptions as the CMakeLists does it. Only seven are arch_*: "the arch boundary" is
// where the seam is CUT, and the sixteen is a property of the chosen source set.
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
#include <kickos/sched.h>
#include <kickos/time.h>

#include "kfixture.h"

extern "C"
{
    arch_irq_state_t arch_irq_save(void)
    {
        return 0;
    }

    void arch_irq_restore(arch_irq_state_t)
    {
    }

    int arch_in_isr(void)
    {
        if (kickos::testfix::g_in_isr)
        {
            return 1;
        }
        return 0;
    }

    void arch_mpu_apply(struct arch_mpu_region const*, size_t)
    {
    }

    void arch_idle_wait(void)
    {
        kickos::testfix::note_park();
    }

    void arch_start(struct arch_context*, struct arch_context*)
    {
    }

    // Returns without switching; see kfixture.h for what that costs an arm.
    void arch_switch(struct arch_context* from, struct arch_context* to)
    {
        kickos::testfix::note_switch(kickos::testfix::thread_of_context(from),
                                     kickos::testfix::thread_of_context(to));
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

    // LOUD, and never with the status it was handed. exit_current calls this when the dying
    // thread was the last live one, and an exit(0) from there would end the gate green with
    // failures already printed and later arms never run: the harness would forge its own
    // only failure signal. An arm that wants the last-thread-out path keeps a spare thread
    // live, exactly as the console arm does.
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

    void domain_release(Domain*)
    {
    }

    void irq_ref_drop(int, bool)
    {
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
