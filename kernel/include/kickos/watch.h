// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The one call that ties an event source to a notification (kos_watch, <kickos/sys.h>). Each kind
// keeps its storage, lifetime and authority in its source's own file: the TASK watch in Task
// (task.cc), the IRQ watch in its IrqBinding (irq.cc).

#ifndef KICKOS_WATCH_H
#define KICKOS_WATCH_H

#include <stdint.h>

namespace kickos
{
    struct Thread; // kickos/thread.h

    // kos_watch for `c`: >= 0, or the first refusal in the order KOS_SYS_WATCH lists.
    int watch_call(Thread* c, uint32_t spec, uintptr_t source, uint32_t notify_cap, uintptr_t arg);
}

#endif
