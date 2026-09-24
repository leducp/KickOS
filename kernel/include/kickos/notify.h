// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The notification object: a word of pending bits, at most one bound waiter, the mask that
// waiter currently accepts, and the chain of signallers that raise into it. A CAP_NOTIFY
// carries a badge, which is the bit a signal through that capability sets; an IRQ binding
// attached with kos_irq_bind_notify is one signaller among others, and a CAP_SIGNAL holder
// calling kos_notify is another. An object with no line attached is a complete notification.
//
// Rights: CAP_WAIT to bind, unbind and wait; CAP_SIGNAL to signal and to be installed as a
// signaller. Claiming a line still needs AUTH_IRQ.

#ifndef KICKOS_NOTIFY_H
#define KICKOS_NOTIFY_H

#include <stdint.h>

#include <kickos/config.h>

namespace kickos
{
    struct Thread; // kickos/thread.h

    // Every "none" sentinel below is zero: a footprint rule, not a style choice. The whole
    // of `struct Kernel` is one statically-allocated object, and a single non-zero default
    // member initialiser anywhere inside it moves the whole object out of .bss and into
    // .data, where its initialiser image costs flash. Measured on f302nucleo: a `-1` default
    // on these three fields alone put 3184 bytes of .bss into .data. So each of them stores a
    // biased value, exactly as kcap_free_ref biases the capability free list for the same
    // reason: zero is the sentinel and every real value is one more than itself.

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

    // A freed slot keeps its last contents (slotpool.h), so notify_create seats all four
    // fields; none of them is a live default. `bound == nullptr` and `accept == 0` are the
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
    };

    // Create a notification and install a full-rights CAP_NOTIFY naming it. 0 with the cap in
    // *out_cap, or -KOS_E*: ENOMEM (pool), EMFILE (cap table), EOVERFLOW (task budget).
    int notify_create(Thread* c, uint32_t* out_cap);

    // Mint a second name for the object `src_cap` already names, badged with `bit` and
    // carrying the source's rights. The badge is set at the copy and never afterwards: an
    // unbadged capability is the unconfined one, a badged copy reaches only its own bit, and
    // `src_cap` itself must be unbadged or a holder could re-reach the whole object. 0 with
    // the new capability in *out_cap, or -KOS_E*: EBADF, EINVAL (bit at or above
    // KCAP_BADGE_BITS), EALREADY (the source is already badged), EMFILE (the caller's
    // cap table), EOVERFLOW (the object's reference count is at its ceiling).
    int notify_badge(Thread* c, uint32_t src_cap, uint32_t bit, uint32_t* out_cap);

    // Raise this capability's badge bit. Requires CAP_SIGNAL. 0, -KOS_EALREADY where the bit
    // was already set (the raise had no effect and the notification stays pending), or
    // -KOS_EBADF / -KOS_EPERM. An unbadged capability raises bit 0.
    int notify_signal(Thread* c, uint32_t cap_handle);

    // Bind the calling thread to the object. Requires CAP_WAIT. 0, -KOS_EBUSY (another
    // thread is bound, or `c` is already bound to a different object), or -KOS_EBADF /
    // -KOS_EPERM. Rebinding the same thread to the same object succeeds. Takes a reference of
    // its own, so closing the last capability while a thread is bound keeps the object alive.
    // A waiter cannot learn its signallers' badges; the accept mask is an argument of the wait.
    int notify_bind(Thread* c, uint32_t cap_handle);

    // Release `c`'s binding and drop the reference it took. Unconsumed bits stay pending for
    // the next server. 0, -KOS_EPERM (`c` is not the bound thread), or -KOS_EBADF.
    int notify_unbind(Thread* c, uint32_t cap_handle);

    // Wait for any bit of `mask`. Requires CAP_WAIT and a binding to `c`. On entry every
    // chained signaller whose badge is in `mask` and which owes a rearm is rearmed, so a
    // driver that never acks cannot deadlock its line. Consumed bits are cleared from pending and
    // written to *out_bits, and their signallers owe a rearm at the next wait.
    // 0, or -KOS_E*: EBADF, EPERM (no CAP_WAIT, `c` is not the bound thread, or above one
    // kernel core its own mask is not exactly its lines' claim core), EINVAL (an empty mask,
    // which could only ever block forever), ETIMEDOUT, ECANCELED.
    // `timeout_us` is relative; KOS_TIMEOUT_NONE waits forever.
    int notify_wait(Thread* c, uint32_t cap_handle, uint32_t mask, uint32_t timeout_us,
                    uint32_t* out_bits);

    // Drop `c`'s binding at its death: clear bound and accept, drop the bind's reference, and
    // leave pending untouched. Runs in exit_current's first locked block, separately from
    // cap_teardown, whose sweep releases IrqLock every KCAP_TEARDOWN_CHUNK slots: an object
    // still naming a thread across one of those gaps would wake a thread already mid-teardown.
    // Caller holds IrqLock.
    void notify_unbind_self(Thread* c);

    // Drop one reference to notification `obj_handle`; free the slot at refs -> 0. Caller
    // holds IrqLock. A parked waiter is the bound thread, whose bind holds a reference of its
    // own, so refs cannot reach 0 under it.
    void notify_ref_drop(int obj_handle, bool teardown);

    // Raise `bit` in `n` and wake the bound thread if its wait accepts that bit. False where
    // the bit was already set. ISR-safe: it resolves nothing, allocates nothing and walks no
    // chain. Takes IrqLock; the signal syscalls nest it.
    bool notify_raise(Notification* n, uint32_t bit);

    // The fused receive's two halves, over whatever object `c` is bound to. enter() opens
    // `mask` (rearming the signallers it covers) and answers the bits the wait may take;
    // leave() consumes the pending bits within `opened`, closes the window and answers what
    // it took. Both answer 0 for a thread bound to nothing. Caller holds IrqLock.
    uint32_t notify_wait_enter(Thread* c, uint32_t mask);
    uint32_t notify_wait_leave(Thread* c, uint32_t opened);

    // The pending word of whatever `c` is bound to, or 0. Caller holds IrqLock.
    uint32_t notify_pending_of(Thread* c);

#if KICKOS_KERNEL_CORES > 1
    // irq_admit_signallers over whatever `c` is bound to, and 0 where it is bound to nothing.
    // Caller holds IrqLock.
    int notify_wait_admit(Thread* c);
#endif
}

#endif
