// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The kernel's record of a mutex word some thread waits on, apart from its operations
// (sync.h), so that an object embedding one need not include the scheduler.

#ifndef KICKOS_MUTEX_H
#define KICKOS_MUTEX_H

#include <kickos/arch/arch.h>
#include <kickos/list.h>

namespace kickos
{
    struct Thread;

    // Lives only while it has a waiter: taken at the first park on a word, freed when its last
    // waiter leaves. While it lives `owner` is the authority on who holds the word, never the
    // word itself; a free record has none. An owner-died hand-over reaches the waiter only
    // through its Thread::wait_result.
    struct Mutex
    {
        // The word's physical address (kickos/word.h).
        arch_phys_addr_t key = 0;
        Thread* owner = nullptr;
        List waiters;
        Mutex* next_held = nullptr;   // intrusive link in the owner's held list
        Mutex* prev_active = nullptr; // intrusive links in the kernel's active list
        Mutex* next_active = nullptr;
    };
}

#endif
