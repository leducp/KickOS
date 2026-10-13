// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The steps the console_held arms share: a publish and a thread-fault record.

#ifndef KICKOS_TESTS_UNIT_CONSOLEHOLD_HELD_FIXTURE_H
#define KICKOS_TESTS_UNIT_CONSOLEHOLD_HELD_FIXTURE_H

#include <gtest/gtest.h>

#include <string>

#include <kickos/console_tx.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sync.h>

#include <stdio.h>

#include "console_seam.h"
#include "isolated_arm.h"

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

    // The n-th of a few threads a record is printed by.
    inline kickos::Thread* thread(uintptr_t n)
    {
        static kickos::Thread threads[160];
        return &threads[n];
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
        consoleseam::publish_begin();
        consoleseam::publish_end();
    }

    // What the end of the console's task does (cap_console_task_ended).
    inline void task_ends()
    {
        kickos::IrqLock lock;
        console_note_driver_death();
        console_on_driver_death(lock);
    }
}

#endif
