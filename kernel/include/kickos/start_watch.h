// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The INSTANCE watch (kos_watch, <kickos/sys.h>): "this declared instance can start again". It
// is a CAP_WATCH entry of the arming thread's own table, holding a counted reference on the
// notification it raises, and a thread holds at most one per declaration. From its arming to its
// close it is on its declaration's list (Kernel::start_watches), doubly linked through the
// entries' obj_gen words; a raise leaves it there, raised, for its holder's close or re-arm.

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
    // -KOS_EOVERFLOW (the object's reference count, which a re-arm onto the object its watch
    // holds already does not take) or -KOS_EMFILE (the table). Where `c` holds a watch on `decl`
    // already, armed or raised, it is re-armed in place under a new handle, the old one stale,
    // and a refusal leaves it as it was. O(the watches on `decl`). Caller holds IrqLock.
    int start_watch_arm(Thread* c, size_t decl, int notify_obj, uint32_t bit, uint32_t* out_cap,
                        Held held);

    // `e`, a CAP_WATCH being closed or swept, leaves its declaration's list. O(1) but for the
    // recompute of a dying declaration's threads where `e` was armed. Caller holds IrqLock.
    void start_watch_close(CapEntry& e, Held held);

    // Raises every armed watch whose declaration is clear for no create in particular, then
    // takes the pass the highest waiter it woke is owed. Interrupt-safe: it frees nothing. Takes
    // IrqLock, then O(the declarations) with one clear test per declaration holding an armed
    // watch, plus one raise per armed watch of a clear one.
    void start_watch_poke();

    // Marks the notification of every watch armed on `decl` (notify_lend_mark): whether there was
    // one. O(the watches on `decl`). Caller holds IrqLock.
    bool start_watch_mark_decl(size_t decl);

    // Owes a recompute (task_watch_lend_owe) to the ended record of every dying declaration a
    // watch armed on raises into notification `notify_obj`: whether there was one. O(the
    // declarations + the watches armed on the dying ones). Caller holds IrqLock.
    bool start_watch_owe_into(int notify_obj);

    // The highest priority of a thread parked on a bit a watch armed on `decl` raises, else 0.
    // O(the watches on `decl`). Caller holds IrqLock.
    uint8_t start_watch_lend(size_t decl);
}

#endif
