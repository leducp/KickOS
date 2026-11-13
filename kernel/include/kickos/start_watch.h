// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The INSTANCE watch (kos_watch, <kickos/sys.h>): "this declared instance can start again". It
// is a CAP_WATCH entry of the arming thread's own table, holding a counted reference on the
// notification it raises, and, while armed, a link on Kernel::start_watches threaded through
// the entries' obj_gen words. A raise unlinks it; the entry stays for its holder's close.

#ifndef KICKOS_START_WATCH_H
#define KICKOS_START_WATCH_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/held.h>

namespace kickos
{
    struct CapEntry; // kickos/cap.h
    struct Task;     // kickos/task.h
    struct Thread;   // kickos/thread.h

    // Arms a watch raising bit `bit` of notification `notify_obj` once declaration `decl` is
    // clear (task_decl_clear), in `c`'s table: 0 and the capability handle in *out_cap, or
    // -KOS_EOVERFLOW (the object's reference count) or -KOS_EMFILE (the table). Caller holds
    // IrqLock.
    int start_watch_arm(Thread* c, size_t decl, int notify_obj, uint32_t bit, uint32_t* out_cap,
                        Held held);

    // `e`, a CAP_WATCH being closed or swept, is unlinked where it is still armed. Caller holds
    // IrqLock.
    void start_watch_close(CapEntry& e, Held held);

    // Raises and unlinks every armed watch whose declaration is clear for no create in particular,
    // then takes the pass the highest waiter it woke is owed. Interrupt-safe: it frees nothing.
    // Takes IrqLock, then one load with none armed, else O(the armed watches).
    void start_watch_poke();

    // Marks the notification of every watch armed on `decl` (notify_lend_mark): whether there was
    // one. Caller holds IrqLock.
    bool start_watch_mark_decl(size_t decl);

    // Owes a recompute (task_watch_lend_owe) to the ended record of every watch raising into
    // notification `notify_obj`: whether there was one. Caller holds IrqLock.
    bool start_watch_owe_into(int notify_obj);

    // `p`, or the highest priority of a thread parked on a bit a watch armed on `t`'s
    // declaration raises. O(the armed watches). Caller holds IrqLock.
    uint8_t start_watch_lend(Task const* t, uint8_t p);
}

#endif
