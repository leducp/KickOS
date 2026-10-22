// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The kernel's record of a user word some thread waits on, apart from its operations (sync.h),
// so that an object embedding one need not include the scheduler.

#ifndef KICKOS_WORD_RECORD_H
#define KICKOS_WORD_RECORD_H

#include <kickos/arch/arch.h>
#include <kickos/list.h>

#include <stdint.h>

namespace kickos
{
    struct Thread;

    // FREE is the one test of a free record. A PI record is a priority-inheritance mutex's; a
    // PLAIN one is a plain word's (KOS_SYS_WORD_WAIT), which no call of the other kind accepts.
    enum class WordKind : uint8_t
    {
        FREE = 0,
        PI,
        PLAIN
    };

    // Lives only while it has a waiter: taken at the first park on a word, freed when its last
    // waiter leaves. While a PI record lives `owner` is the authority on who holds the word,
    // never the word itself. An owner-died hand-over reaches the waiter only through its
    // Thread::wait_result. `owner` and `next_held` are a PI record's alone.
    struct WordRecord
    {
        // The word's physical address (kickos/word.h).
        arch_phys_addr_t key = 0;
        Thread* owner = nullptr;
        List waiters;
        WordRecord* next_held = nullptr;   // intrusive link in the owner's held list
        WordRecord* prev_active = nullptr; // intrusive links in the kernel's active list
        WordRecord* next_active = nullptr;
        WordKind kind = WordKind::FREE;
    };
}

#endif
