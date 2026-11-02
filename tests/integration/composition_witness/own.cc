// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A word each task's image holds its own copy of, zero at every start of the task: a task that
// finds it set at its start, or set by another task later, prints "<task>: statics not its own".
#include <stdint.h>
#include <stdio.h>

namespace
{
    uint32_t volatile g_owner;
}

extern "C" void witness_own_claim(char const* task, uint32_t id)
{
    if (g_owner != 0u)
    {
        printf("%s: statics not its own (%lu at its start)\n", task, static_cast<unsigned long>(g_owner));
    }
    g_owner = id;
}

extern "C" void witness_own_check(char const* task, uint32_t id)
{
    if (g_owner != id)
    {
        printf("%s: statics not its own (%lu)\n", task, static_cast<unsigned long>(g_owner));
    }
}
