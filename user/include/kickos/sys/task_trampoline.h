// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The entry thread the init spawns for each user task, its argument the task's table record.

#ifndef KICKOS_SYS_TASK_TRAMPOLINE_H
#define KICKOS_SYS_TASK_TRAMPOLINE_H

namespace kickos::init
{
    // Calls the record's entry with the record as `self`; when it returns, flushes stdout and
    // stderr and ends the thread with kos_exit(0).
    [[noreturn]] void task_trampoline(void* self);
}

#endif
