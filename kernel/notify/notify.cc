// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/notify.h>

#include <kickos/cap.h>
#include <kickos/debug.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/time.h> // ktime_deadline_arm, for the timed wait
#include <kickos/bench.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

namespace kickos
{
    namespace
    {
        // Caller holds IrqLock.
        Notification* notify_of_cap(Thread* c, uint32_t cap_handle, uint8_t need, int* err)
        {
            return static_cast<Notification*>(
                cap_resolve_e(c, cap_handle, CapType::CAP_NOTIFY, need, err));
        }

        // The badge a capability carries, as a BIT and not as the stored bit+1. An unbadged
        // capability answers bit 0, which is total: every object has a bit 0.
        uint32_t badge_bit_of(CapEntry const& e)
        {
            uint8_t const stored = cap_badge(e);
            if (stored == KCAP_BADGE_NONE)
            {
                return 0;
            }
            return static_cast<uint32_t>(stored) - 1u;
        }

        // Caller holds IrqLock.
        Notification* bound_object_of(Thread* c)
        {
            if (c == nullptr or c->notify_bound == KOS_NOTIFY_UNBOUND)
            {
                return nullptr;
            }
            return kernel().notifies.resolve(notify_bound_handle(c->notify_bound));
        }
    }

    int notify_create(Thread* c, uint32_t* out_cap)
    {
        IrqLock lock;
        *out_cap = KCAP_INVALID;
        if (c == nullptr)
        {
            return -KOS_EPERM;
        }
        Kernel& k = kernel();
        // Before the pool, as at the other object creators: a task at its ceiling is refused
        // without churning a slot.
        if (not task_object_admit(CapType::CAP_NOTIFY, c->task))
        {
            return -KOS_EOVERFLOW;
        }
        int const i = k.notifies.alloc();
        Notification* const n = k.notifies.at(i); // total over alloc()'s -1: one refusal below
        if (n == nullptr)
        {
            return -KOS_ENOMEM;
        }
        // A freed slot keeps its last occupant's fields, so all four are seated here.
        n->pending = 0;
        n->bound = nullptr;
        n->accept = 0;
        n->signallers = NOTIFY_SIGNALLER_NONE;
        k.notify_refs[i] = 1; // the creator's cap is the first reference
        int const obj = k.notifies.handle_for(i);
        uint32_t cap = KCAP_INVALID;
        int const rc = cap_install(c, obj, CapType::CAP_NOTIFY,
                                   CAP_WAIT | CAP_SIGNAL | CAP_TRANSFER, &cap);
        if (rc != 0)
        {
            k.notify_refs[i] = 0;
            k.notifies.free(obj);
            return rc;
        }
        *out_cap = cap;
        return 0;
    }

    int notify_badge(Thread* c, uint32_t src_cap, uint32_t bit, uint32_t* out_cap)
    {
        IrqLock lock;
        *out_cap = KCAP_INVALID;
        if (bit >= KCAP_BADGE_BITS)
        {
            return -KOS_EINVAL;
        }
        CapEntry const* const e = cap_lookup(c, src_cap);
        if (e == nullptr or e->type != static_cast<uint8_t>(CapType::CAP_NOTIFY))
        {
            return -KOS_EBADF;
        }
        // Refused, not re-seated: a holder able to mint from a badged capability reaches
        // every bit of the object and the confinement a badge exists for is vacuous.
        if (cap_badge(*e) != KCAP_BADGE_NONE)
        {
            return -KOS_EALREADY;
        }
        int const obj = e->obj;
        uint8_t const rights = e->rights;
        // The reference comes BEFORE the slot: a refused bump must leave no capability.
        if (not obj_ref_inc(CapType::CAP_NOTIFY, obj, rights))
        {
            return -KOS_EOVERFLOW;
        }
        uint32_t const index = cap_run_peek_free(c->cap_free_head);
        if (index == KCAP_NO_SLOT)
        {
            obj_ref_undo(CapType::CAP_NOTIFY, obj, rights);
            return -KOS_EMFILE;
        }
        CapEntry const* const ne =
            cap_install_at(c, static_cast<int>(index), obj, CapType::CAP_NOTIFY, rights,
                           static_cast<uint8_t>(bit + 1u));
        *out_cap = (static_cast<uint32_t>(ne->gen) << KCAP_INDEX_BITS) | index;
        return 0;
    }

    bool notify_raise(Notification* n, uint32_t bit)
    {
        IrqLock lock;
        bool const changed = (n->pending & bit) == 0u;
        n->pending = n->pending | bit;
        Thread* const t = n->bound;
        if (t == nullptr)
        {
            return changed; // latched for whatever server binds next
        }
        if ((n->accept & bit) == 0u)
        {
            return changed; // no wait open on this bit: it stays pending
        }
        if (t->wait_notify() == n)
        {
            // Normal delivery preserves wait_result; only an early wake changes it.
            t->clear_wait_edge();
            sched::wake(t);
            return changed;
        }
        // Wake an accepting fused receive with -KOS_ENOTIFY; its opts carry the taken bits.
        if (t->state == ThreadState::BLOCKED and t->wait_kind == WAIT_EP_RECV)
        {
            t->wait_queue->unlink(&t->link);
            t->clear_wait_edge();
            t->wait_result = -KOS_ENOTIFY;
            sched::wake(t);
            return changed;
        }
        return changed;
    }

    int notify_signal(Thread* c, uint32_t cap_handle)
    {
        IrqLock lock;
        int err = 0;
        Notification* const n = notify_of_cap(c, cap_handle, CAP_SIGNAL, &err);
        if (n == nullptr)
        {
            return -err; // EBADF (bad/closed cap, freed slot) or EPERM (no SIGNAL right)
        }
        // cap_lookup cannot fail here: the resolve above went through it.
        CapEntry const* const e = cap_lookup(c, cap_handle);
        uint32_t const bit = 1u << badge_bit_of(*e);
        // No controller access here: a signal must never unmask an unserviced line, and a
        // waiter must be idempotent about finding no work.
        if (not notify_raise(n, bit))
        {
            return -KOS_EALREADY;
        }
        return 0;
    }

    int notify_bind(Thread* c, uint32_t cap_handle)
    {
        IrqLock lock;
        int err = 0;
        Notification* const n = notify_of_cap(c, cap_handle, CAP_WAIT, &err);
        if (n == nullptr)
        {
            return -err;
        }
        int const obj = static_cast<int>(cap_lookup(c, cap_handle)->obj);
        if (n->bound == c)
        {
            return 0; // idempotent, and it must not take a second reference
        }
        if (n->bound != nullptr)
        {
            return -KOS_EBUSY; // one bound waiter per object
        }
        if (c->notify_bound != KOS_NOTIFY_UNBOUND)
        {
            return -KOS_EBUSY; // one object per thread: the TCB names exactly one
        }
        // The bind takes a reference of its own: leaning on the binder's capability would
        // hand a stranger this object the moment that capability closed.
        if (not obj_ref_inc(CapType::CAP_NOTIFY, obj, 0))
        {
            return -KOS_EOVERFLOW;
        }
        n->bound = c;
        n->accept = 0;
        c->notify_bound = notify_bound_store(obj);
        return 0;
    }

    namespace
    {
        // Clear the binding `c` holds on `n` and drop the reference it took. Caller holds
        // IrqLock and has established that n->bound == c.
        void unbind_locked(Thread* c, Notification* n, int obj, bool teardown)
        {
            n->bound = nullptr;
            n->accept = 0;
            c->notify_bound = KOS_NOTIFY_UNBOUND;
            // pending is left: it is the next server's, exactly as it was the last one's.
            notify_ref_drop(obj, teardown);
        }
    }

    int notify_unbind(Thread* c, uint32_t cap_handle)
    {
        IrqLock lock;
        int err = 0;
        Notification* const n = notify_of_cap(c, cap_handle, CAP_WAIT, &err);
        if (n == nullptr)
        {
            return -err;
        }
        if (n->bound != c)
        {
            return -KOS_EPERM;
        }
        unbind_locked(c, n, static_cast<int>(cap_lookup(c, cap_handle)->obj), false);
        return 0;
    }

    void notify_unbind_self(Thread* c)
    {
        if (c == nullptr or c->notify_bound == KOS_NOTIFY_UNBOUND)
        {
            return;
        }
        int const obj = notify_bound_handle(c->notify_bound);
        Notification* const n = kernel().notifies.resolve(obj);
        if (n == nullptr)
        {
            // Unreachable under correct refcounting: the bind's own reference keeps the slot
            // live. Clear the name anyway rather than leave a handle nothing backs.
            c->notify_bound = KOS_NOTIFY_UNBOUND;
            return;
        }
        KICKOS_DEBUG_ASSERT(n->bound == c);
        unbind_locked(c, n, obj, true);
    }

    void notify_ref_drop(int obj_handle, bool teardown)
    {
        (void)teardown;
        Kernel& k = kernel();
        int const idx = k.notifies.live_index(obj_handle);
        if (idx < 0)
        {
            return; // already gone: cannot happen under correct refcounting
        }
        uint8_t& r = k.notify_refs[idx];
        if (r > 0)
        {
            r--;
        }
        if (r == 0)
        {
            // A waiter parked here is the bound thread, and its bind holds a reference of its
            // own, so this count cannot reach zero under it. The same fact is what lets the
            // last IRQ capability close while a driver is parked: the driver is parked on the
            // object, not on the line.
            Notification const* const n = k.notifies.at(idx);
            KICKOS_ASSERT(n == nullptr or n->bound == nullptr);
            KICKOS_ASSERT(n == nullptr or n->signallers == NOTIFY_SIGNALLER_NONE);
            k.notifies.free(obj_handle);
        }
    }

    uint32_t notify_pending_of(Thread* c)
    {
        Notification const* const n = bound_object_of(c);
        if (n == nullptr)
        {
            return 0;
        }
        return n->pending;
    }

#if KICKOS_KERNEL_CORES > 1
    int notify_wait_admit(Thread* c)
    {
        Notification const* const n = bound_object_of(c);
        if (n == nullptr)
        {
            return 0;
        }
        return irq_admit_signallers(c, n);
    }
#endif

    uint32_t notify_wait_enter(Thread* c, uint32_t mask)
    {
        Notification* const n = bound_object_of(c);
        if (n == nullptr or mask == 0)
        {
            return 0;
        }
        // Rearm before the window opens, exactly as the standalone wait does: a line whose
        // event the previous pass consumed is still masked until here.
        irq_signallers_rearm(n, mask);
        n->accept = mask;
        return mask;
    }

    uint32_t notify_wait_leave(Thread* c, uint32_t opened)
    {
        Notification* const n = bound_object_of(c);
        if (n == nullptr)
        {
            return 0;
        }
        n->accept = 0;
        uint32_t const taken = n->pending & opened;
        n->pending = n->pending & ~taken;
        // Flag for rearm here, never in the ISR: that is what makes ack;compute;wait
        // phantom-free.
        irq_signallers_owe_rearm(n, taken);
        return taken;
    }

    // The one cancellation point in the kernel. Do not fold back into sem_wait: sem_wait
    // returns void and never reads wait_result, so an early wake would look like a post.
    int notify_wait(Thread* c, uint32_t cap_handle, uint32_t mask, uint32_t timeout_us,
                    uint32_t* out_bits)
    {
        *out_bits = 0;
        if (mask == 0)
        {
            return -KOS_EINVAL; // a wait nothing can ever satisfy
        }
        Notification* n = nullptr;
        uint32_t epoch = 0;
        {
            IrqLock lock;
            int err = 0;
            n = notify_of_cap(c, cap_handle, CAP_WAIT, &err);
            if (n == nullptr)
            {
                return -err; // EBADF (bad/closed cap, freed slot) or EPERM (no WAIT right)
            }
#if KICKOS_KERNEL_CORES > 1
            int const arc = irq_admit_signallers(c, n);
            if (arc != 0)
            {
                return arc;
            }
#endif
            // Cancellation takes precedence over a missing binding.
            if (park_cancel_pending(c))
            {
                return -KOS_ECANCELED;
            }
            if (n->bound != c)
            {
                return -KOS_EPERM; // only the bound thread receives from this object
            }
            irq_signallers_rearm(n, mask);
            uint32_t const ready = n->pending & mask;
            if (ready != 0)
            {
                n->pending = n->pending & ~ready;
                irq_signallers_owe_rearm(n, ready);
                *out_bits = ready;
                return 0;
            }
            n->accept = mask;
            // `n` survives the park: this thread's own bind holds a reference on the object,
            // so nothing another thread can close frees it under a parked waiter.
            c->wait_result = 0; // normal delivery leaves this result unchanged
            epoch = c->switch_count;
            // Under this lock and ahead of the block: the raise that wakes this thread takes
            // the same lock, so nothing delivered past this mark can arrive before the park.
            KICKOS_BENCH_E2E_PARK_MARK();
            // WAIT_NOTIFY supports early wake results. No queue is needed for one bound
            // waiter.
            park_queueless(c, WAIT_NOTIFY, n);
            if (timeout_us != KOS_TIMEOUT_NONE)
            {
                ktime_deadline_arm(c, timeout_us);
            }
            sched::reschedule();
        }
        // Mandatory, and outside the lock: where the switch is only pended when the block
        // scope's lock drops, a wait_result read before this returns the pre-block value.
        wq_confirm_resume(c, epoch);
        {
            IrqLock lock;
            n->accept = 0;
            if (c->wait_result != 0)
            {
                // Left as the rearm above set it: the exiting thread's cap drop detaches
                // and masks the line.
                return static_cast<int>(c->wait_result);
            }
            uint32_t const ready = n->pending & mask;
            n->pending = n->pending & ~ready;
            // Flag for rearm here, never in the ISR: that is what makes ack;compute;wait
            // phantom-free.
            irq_signallers_owe_rearm(n, ready);
            *out_bits = ready;
        }
        return 0;
    }
}
