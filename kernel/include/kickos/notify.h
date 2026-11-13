// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The notification object: a word of pending bits, at most one bound waiter, the mask that
// waiter currently accepts, and the chain of signallers that raise into it. A CAP_NOTIFY
// carries a badge, which is the bit a signal through that capability sets; an IRQ binding
// attached with a KOS_WATCH_IRQ watch is one signaller among others, and a CAP_SIGNAL holder
// calling kos_notify is another. An object with no line attached is a complete notification.
//
// Rights: CAP_WAIT to bind, unbind and wait; CAP_SIGNAL to signal and to be installed as a
// signaller. Claiming a line still needs AUTH_IRQ.

#ifndef KICKOS_NOTIFY_H
#define KICKOS_NOTIFY_H

#include <stdint.h>

#include <kickos/config.h>
#include <kickos/held.h>
#include <kickos/time.h>

namespace kickos
{
    struct Thread;   // kickos/thread.h
    struct CapEntry; // kickos/cap.h

    // Every "none" sentinel below is zero. `struct Kernel` is one statically-allocated object,
    // and a single non-zero default member initialiser anywhere inside it moves the whole
    // object out of .bss and into .data, where its initialiser image costs flash. So each of
    // them stores a biased value, as kcap_free_ref does: zero is the sentinel and every real
    // value is one more than itself.

    // "No signaller on this chain". The chain threads IrqBinding::next_signaller, whose
    // entries are binding pool indices biased by one, not handles: the chain is walked only
    // under IrqLock in thread context, where a freed slot has already been unlinked.
    static constexpr uint8_t NOTIFY_SIGNALLER_NONE = 0;

    inline uint8_t notify_signaller_ref(int index)
    {
        return static_cast<uint8_t>(index + 1);
    }
    inline int notify_signaller_index(uint8_t ref)
    {
        return static_cast<int>(ref) - 1;
    }

    // "This thread is bound to nothing", in Thread::notify_bound, which stores the object's
    // generational handle biased by one. The unbiased zero would be slot 0 at generation 0,
    // the first handle the pool ever hands out, so a freshly zeroed TCB would read as bound
    // to it. Only the all-ones handle biases to zero, and SlotPool's reserved index keeps it
    // unmintable.
    static constexpr int32_t KOS_NOTIFY_UNBOUND = 0;

    inline int32_t notify_bound_store(int handle)
    {
        return static_cast<int32_t>(static_cast<uint32_t>(handle) + 1u);
    }
    inline int notify_bound_handle(int32_t stored)
    {
        return static_cast<int>(static_cast<uint32_t>(stored) - 1u);
    }

    // A freed slot keeps its last contents (slotpool.h), so notify_create seats every field but
    // hold_mark; none of them is a live default. `bound == nullptr` and `accept == 0` are the
    // null cases and carry no sentinel beside them.
    struct Notification
    {
        // Raised bits nothing has consumed. Survives the bound thread's death, for the next
        // server.
        uint32_t pending = 0;
        // The one waiter. A second bind is refused while this is set to another thread.
        Thread* bound = nullptr;
        // What the current wait may be woken by. Set at wait entry and cleared at its exit,
        // under IrqLock. A bit raised outside it stays pending and wakes nobody.
        uint32_t accept = 0;
        // Head of the signaller chain, a biased binding pool index, or NOTIFY_SIGNALLER_NONE.
        uint8_t signallers = NOTIFY_SIGNALLER_NONE;
        // Non-zero once an ended task may have a watch on its death raising here, so a wait
        // parking or woken finds what it lends to; cleared by a look that finds none. In the
        // padding before hold_mark.
        uint8_t lend_mark = 0;
        // The holdings epoch that last counted this slot (cap.cc), in the tail padding.
        uint16_t hold_mark = 0;
    };
    static_assert(sizeof(Notification) == 2 * sizeof(void*) + 8,
                  "lend_mark and hold_mark sit in padding: 16 bytes at 32 bits, 24 at 64");

    // Create a notification and install a full-rights CAP_NOTIFY naming it. 0 with the cap in
    // *out_cap, or -KOS_E*: ENOMEM (pool), EMFILE (cap table), EOVERFLOW (task budget).
    int notify_create(Thread* c, uint32_t* out_cap);

    // Mint a second name for the object `src_cap` already names, badged with `bit` and
    // carrying the source's rights. The badge is set at the copy and never afterwards: an
    // unbadged capability is the unconfined one, a badged copy reaches only its own bit, and
    // `src_cap` itself must be unbadged or a holder could re-reach the whole object. 0 with
    // the new capability in *out_cap, or -KOS_E*: EBADF, EINVAL (bit at or above
    // KCAP_BADGE_BITS), EACCES (a badged source cannot mint), EMFILE (the caller's
    // cap table), EOVERFLOW (the object's reference count is at its ceiling).
    int notify_badge(Thread* c, uint32_t src_cap, uint32_t bit, uint32_t* out_cap);

    // Raise this capability's badge bit. Requires CAP_SIGNAL. 0, -KOS_EALREADY where the bit
    // was already set (the raise had no effect and the notification stays pending), or
    // -KOS_EBADF / -KOS_EACCES. An unbadged capability raises bit 0.
    int notify_signal(Thread* c, uint32_t cap_handle);

    // Bind the calling thread to the object. Requires CAP_WAIT. 0, -KOS_EBUSY (another
    // thread is bound, or `c` is already bound to a different object), or -KOS_EBADF /
    // -KOS_EACCES. Rebinding the same thread to the same object succeeds. Takes a reference of
    // its own, so closing the last capability while a thread is bound keeps the object alive.
    // A waiter cannot learn its signallers' badges; the accept mask is an argument of the wait.
    int notify_bind(Thread* c, uint32_t cap_handle);

    // Release `c`'s binding and drop the reference it took. Unconsumed bits stay pending for
    // the next server. 0, -KOS_EPERM (`c` is not the bound thread), or -KOS_EBADF.
    int notify_unbind(Thread* c, uint32_t cap_handle);

    // Wait for any bit of `mask` on the notification `c` is bound to, until the monotonic
    // `deadline`. On entry
    // every chained signaller whose badge is in `mask` and which owes a rearm is rearmed, so a
    // driver that never acks cannot deadlock its line. The bits taken are cleared from pending,
    // their signallers owing a rearm at the next wait, and answered in the high half:
    // notify_wait_bits. Else -KOS_E*: EINVAL (an empty mask, which could only ever block
    // forever), EPERM (`c` is bound to nothing, or above one kernel core its own mask is not
    // exactly its lines' claim core), ETIMEDOUT, ECANCELED.
    int64_t notify_wait(Thread* c, uint32_t mask, uint64_t deadline);
    // The answer of a wait that took `bits`: 0 in the low half, the bits in the high one.
    constexpr int64_t notify_wait_bits(uint32_t bits)
    {
        return static_cast<int64_t>(static_cast<uint64_t>(bits) << 32);
    }

    // Drop `c`'s binding at its death: clear bound and accept, drop the bind's reference, and
    // leave pending untouched. Runs in exit_current's first locked block, separately from
    // cap_teardown, whose sweep releases IrqLock every KCAP_TEARDOWN_CHUNK slots: an object
    // still naming a thread across one of those gaps would wake a thread already mid-teardown.
    // Caller holds IrqLock.
    void notify_unbind_self(Thread* c, Held held);

    // Drop one reference to notification `obj_handle`; free the slot at refs -> 0. Caller
    // holds IrqLock. A parked waiter is the bound thread, whose bind holds a reference of its
    // own, so refs cannot reach 0 under it.
    void notify_ref_drop(int obj_handle);

    // Raise `bit` in `n` and wake the bound thread if its wait accepts that bit. False where
    // the bit was already set. ISR-safe: it resolves nothing, allocates nothing and walks no
    // chain. Takes IrqLock; the signal syscalls nest it.
    bool notify_raise(Notification* n, uint32_t bit);

    // The same raise from a path that may not reschedule yet: the waiter is woken without a
    // switch, and returned when it should run before the caller, for the caller's deferred
    // wake to take. Null otherwise.
    Thread* notify_raise_deferred(Notification* n, uint32_t bit);

    // The bit INDEX a CAP_NOTIFY capability raises: its badge's, or 0 unbadged.
    uint32_t notify_cap_bit(CapEntry const& e);

    // The fused receive's two halves, over whatever object `c` is bound to. enter() opens
    // `mask` (rearming the signallers it covers) and answers the bits the wait may take, 0 for
    // a thread bound to nothing; the receive's park then calls notify_lend_window. leave()
    // closes the window and writes the pending bits within `opened` to `c`'s word at `out`,
    // taking them only once written: 0, or -KOS_EFAULT with none taken. Bound to nothing, it
    // writes nothing. leave()'s caller holds IrqLock.
    uint32_t notify_wait_enter(Thread* c, uint32_t mask, Held held);
    int notify_wait_leave(Thread* c, uint32_t opened, uintptr_t out, Held held);
    // `c` parked, ahead of its switch: where a window on its bound notification is open, what it
    // lends through a watch on an ended task is recomputed (notify_lend_owe). Caller holds IrqLock.
    void notify_lend_window(Thread* c, Held held);
    // `t` left a park, by whatever wake: where a window on its bound notification is still open,
    // what it lent through that window is owed a recompute (notify_lend_owe), which the caller
    // settles (watch_lend_settle) before `t` runs. Whether one was owed. One byte test where no
    // ended task is watched into the object. Caller holds IrqLock.
    bool notify_lend_woken(Thread* t);

    // The pending word of whatever `c` is bound to, or 0. Caller holds IrqLock.
    uint32_t notify_pending_of(Thread* c);

    // An ended task's watch on its death raises into `n`. Null-safe. Caller holds IrqLock.
    void notify_lend_mark(Notification* n);
    // The priority of the thread parked on `n` with a window open on a bit of `mask`, in
    // kos_notify_wait or a fused receive, else 0. Caller holds IrqLock.
    uint8_t notify_lender_prio(Notification const* n, uint32_t mask);
    // `w` parked or left its park with a window open on its bound notification, or its priority
    // moved while parked: every task a watch on whose death raises there is owed a recompute
    // (task_watch_lend_owe), and the mark goes where there is none. Whether one was owed. O(the
    // task pool + the declarations + the INSTANCE watches armed on the dying ones) where the
    // object is marked, else O(1). Caller holds IrqLock.
    bool notify_lend_owe(Thread* w);

#if KICKOS_KERNEL_CORES > 1
    // irq_admit_signallers over whatever `c` is bound to, and 0 where it is bound to nothing.
    // Caller holds IrqLock.
    int notify_wait_admit(Thread* c);
#endif
}

#endif
