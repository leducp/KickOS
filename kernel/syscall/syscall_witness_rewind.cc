// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KOS_SYS_WITNESS_REWINDS: how many times the switcher moved a thread's stacked PC from inside
// kickos_user_cas's window back to its load. Built only into the rewind witness's own kernel.

#include <stdint.h>

#include "syscall_internal.h"

#if not KICKOS_WITNESS_REWIND
#error "syscall_witness_rewind.cc is the rewind witness's alone"
#endif

// The switcher's assembly is its only writer.
extern "C"
{
    uint32_t volatile g_kickos_witness_rewinds = 0;
}

namespace kickos
{
    int64_t witness_rewinds()
    {
        return static_cast<int64_t>(g_kickos_witness_rewinds);
    }
}
