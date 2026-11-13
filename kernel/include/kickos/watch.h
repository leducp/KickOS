// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The one call that ties an event source to a notification (kos_watch, <kickos/sys.h>). Each kind
// keeps its storage, lifetime and authority in its source's own file: the TASK watch in Task
// (task.cc), the IRQ watch in its IrqBinding (irq.cc), the INSTANCE watch in a capability of the
// caller's table (start_watch.cc), its authority a create's (syscall_thread.cc).

#ifndef KICKOS_WATCH_H
#define KICKOS_WATCH_H

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
    struct Thread; // kickos/thread.h

    // kos_watch for `c`: 0, or the first refusal in the order KOS_SYS_WATCH lists. An INSTANCE
    // watch's handle goes to *out_watch, which no other kind touches.
    int watch_call(Thread* c, uint32_t spec, uintptr_t source, uint32_t notify_cap, uintptr_t arg,
                   uint32_t* out_watch);

    // The declaration of program capability `prog`'s instance named by the user string at
    // `name`, admitted to `c` exactly as a create of it is, into *decl: 0, or -KOS_EBADF,
    // -KOS_EPERM (no KOS_AUTH_TASKS), -KOS_EFAULT, -KOS_EINVAL, -KOS_ENOENT (the name), -KOS_EPERM
    // (the declaration's admission, or one that starts once and has started). Caller holds
    // IrqLock.
    int instance_watch_source(Thread* c, uint32_t prog, uintptr_t name, size_t* decl);
}

#endif
