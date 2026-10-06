// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The steps the console_held arms share: a publish, a thread-fault record, and a forked arm.

#ifndef KICKOS_TESTS_UNIT_CONSOLEHOLD_HELD_FIXTURE_H
#define KICKOS_TESTS_UNIT_CONSOLEHOLD_HELD_FIXTURE_H

#include <gtest/gtest.h>

#include <string>

#include <kickos/console_tx.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>

#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "console_seam.h"
#include "held_seam.h"

namespace heldfix
{
    inline constexpr char kHead[] = "\n=== THREAD FAULT === thread '%s' killed\n";
    inline constexpr char kPc[] = "  PC=0x%x\n";
    inline constexpr char kAddr[] = "  ADDR=0x%x\n";

    inline std::string text(char const* who, unsigned pc)
    {
        char b[160];
        snprintf(b, sizeof(b),
                 "\n=== THREAD FAULT === thread '%s' killed\n  PC=0x%x\n  ADDR=0x%x\n", who, pc,
                 pc + 1u);
        return b;
    }

    inline void record(char const* who, unsigned pc)
    {
        kickos::kprintf_fault(kHead, who);
        kickos::kprintf_fault(kPc, pc);
        kickos::kprintf_fault(kAddr, pc + 1u);
        kickos::krecord_end();
    }

    // A thread's identity; the seam never dereferences it.
    inline kickos::Thread* thread(uintptr_t n)
    {
        return reinterpret_cast<kickos::Thread*>(n * 0x100u);
    }

    // The committed record the driver would receive next.
    inline std::string held()
    {
        kickos::IrqLock lock;
        uint32_t const n = console_held_ready();
        if (n == 0)
        {
            return "";
        }
        return std::string(console_held_data(), n);
    }

    inline void take(size_t n)
    {
        kickos::IrqLock lock;
        console_held_take(static_cast<uint32_t>(n));
    }

    // KOS_SYS_CONSOLE_PUBLISH's console sequence, with no writer in flight to drain.
    inline void publish()
    {
        {
            kickos::IrqLock lock;
            console_handover_begin();
        }
        consoleseam::set_isr_runs_in_gap(false);
        console_owner_set_user();
        consoleseam::note_commit();
    }

    // What the end of the console's task does (cap_console_task_ended).
    inline void task_ends()
    {
        kickos::IrqLock lock;
        console_note_driver_death();
        console_on_driver_death();
    }

    // The ownership state is a one-way street, so each arm gets its own process.
    inline void run_isolated(void (*body)())
    {
        fflush(nullptr);
        pid_t const pid = fork();
        ASSERT_NE(pid, -1) << "fork failed, the arm did not run";
        if (pid == 0)
        {
            body();
            fflush(nullptr);
            if (::testing::Test::HasFailure())
            {
                _exit(1);
            }
            _exit(0);
        }
        int status = 0;
        ASSERT_EQ(waitpid(pid, &status, 0), pid);
        ASSERT_TRUE(WIFEXITED(status)) << "the arm died on a signal";
        ASSERT_EQ(WEXITSTATUS(status), 0) << "see the arm's own failure text above";
    }
}

#endif
