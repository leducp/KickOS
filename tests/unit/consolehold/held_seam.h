// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The recorder held_seam.cc keeps beside the console transport.

#ifndef KICKOS_TESTS_UNIT_CONSOLEHOLD_HELD_SEAM_H
#define KICKOS_TESTS_UNIT_CONSOLEHOLD_HELD_SEAM_H

#include <stdint.h>

#include <kickos/thread.h>

namespace heldseam
{
    // cap_console_deliver calls, each one a record handed on for a receiver to take.
    extern uint32_t g_deliveries;
    // What sched::current() answers: the thread a record is printed by. Never dereferenced.
    extern kickos::Thread* g_current;
    // What dev_window_free answers for the console's reclaim window.
    extern bool g_window_free;
    // What task_serves_console answers.
    extern bool g_serves_console;
    // arch_console_reclaim calls, and console_dark_wake calls.
    extern uint32_t g_reclaims;
    extern uint32_t g_dark_wakes;
    // Run in place of the dark window's park where set: what the writer's wait answers.
    extern int (*g_dark_wait)(void);
    // Run once by the next arch_console_flush_sync, the first step of a reclaim: what another
    // core does while this one reclaims.
    extern void (*g_flush_hook)(void);
}

#endif
