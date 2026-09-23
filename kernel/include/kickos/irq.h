// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// IRQ delivery has two interfaces:
// - Kernel handlers run directly in ISR context through irq_attach.
// - Userspace drivers hold CAP_IRQ capabilities and attach the line to a notification
//   (kickos/notify.h) through kos_irq_bind_notify. The ISR masks the line and raises the
//   binding's badge bit in that object. The driver services the device, then rearms through
//   the next notify_wait or through irq_ack.
// Claiming a line requires AUTH_IRQ; using it requires capability rights.

#ifndef KICKOS_IRQ_H
#define KICKOS_IRQ_H

#include <stdint.h>

#include <kickos/config.h>

#include <kickos/notify.h>
#include <kickos/sync.h>

#if KICKOS_KERNEL_CORES > 1
#include <kickos/sys/atomic.h>
#endif

namespace kickos
{
    struct Thread; // kickos/thread.h

    using IrqHandler = void (*)(void* arg);

#if KICKOS_KERNEL_CORES > 1
    // The callback pair a dispatch runs for one line.
    struct IrqDispatch
    {
        IrqHandler handler = nullptr;
        void* arg = nullptr;
    };

    // Line -> handler dispatch entry, read by the ISR by index. The word names the
    // publication record holding the whole pair. Index 0 names no record and stands for the
    // line's own null-object default, whose argument is the line the dispatch was entered for.
    struct IrqEntry
    {
        Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE> pub{0u};
    };

    // The pair a dispatch entered for `line` would run.
    IrqDispatch irq_published(int line);
#else
    // Line -> handler dispatch entry, read by the ISR by index.
    struct IrqEntry
    {
        IrqHandler handler = nullptr;
        void* arg = nullptr;
    };
#endif

    // Trigger type of a tier-1 binding. EDGE rearms by bare unmask: a raise latched while
    // masked redelivers, so no pulse is lost. LEVEL discards the latch first, because the
    // driver has already cleared the device by the time it waits or acks: a latch surviving
    // from before that clear would phantom-wake the next wait, while a still-asserted source
    // re-latches on its own.
    enum IrqTrigger : uint8_t
    {
        IRQ_EDGE = 0,
        IRQ_LEVEL = 1
    };

    // Tier 1 binding: a line plus the notification it signals. Latency invariant: the ISR is
    // handed this binding directly as its arg, so it never searches a table in ISR context.
    // That holds only because SlotPool slot addresses are stable for the slot's life.
    // A freed slot keeps its last contents (slotpool.h), so no field below means anything
    // until irq_claim has seated them all. Whether a line is held is tracked by the line's
    // dispatch entry, which names the null-object default while the line is free.
    struct IrqBinding
    {
        // The object this line raises into, and the badge bit it raises there. Both are
        // copied in at attach and read in ISR context, which may resolve no capability and
        // walk no pool. A raw pointer is sound because the binding holds a reference on the
        // object, so the slot cannot be freed under it, and slot addresses are stable.
        Notification* notify = nullptr;
        int line = 0;
        // The next binding on notify->signallers, a biased pool index, or
        // NOTIFY_SIGNALLER_NONE. Zero is the sentinel so a statically-allocated pool stays
        // in .bss (kickos/notify.h).
        uint8_t next_signaller = NOTIFY_SIGNALLER_NONE;
        // Bit index in notify->pending, in [0, KCAP_BADGE_BITS).
        uint8_t badge = 0;
        // Set when a wait consumes the event, under IrqLock. Never set in the ISR:
        // that would allow ack to unmask a level interrupt before the device is serviced.
        bool needs_rearm = false;
        uint8_t trigger = IRQ_EDGE;
        // False until the first arm. That arm discards a latch left from before the
        // line had an owner, whatever the trigger type; a LEVEL binding then keeps
        // discarding on every rearm.
        bool armed_once = false;
#if KICKOS_KERNEL_CORES > 1
        // The core this line was claimed on, which is the core it is routed to, fixed for
        // the binding's life. Read by the bind and the wait's admission, which the arch seam
        // cannot answer for: a controller whose gating state is global reports no owning
        // core, and that says nothing about delivery.
        uint8_t claim_core = 0;
#endif
    };

    static_assert(KICKOS_MAX_IRQ_HANDLES < 255,
                  "a binding pool index biased by one must fit the uint8_t chain link, with "
                  "zero left over for the sentinel");

    // Call once at boot, before any attach or claim.
    void irq_init();
    // IRQs that fired on a line with no driver (masked by the default handler).
    // Best-effort diagnostic, readable outside ISR context.
    uint32_t irq_spurious_count();

    // Tier 2: privileged in-kernel direct handler. Returns false if the line is
    // out of range or already bound (one driver per line); true on success.
    //
    // detach leaves the line masked and always succeeds. Above one kernel core, it keeps the
    // line's retiring record alive until no dispatch can still hold it, refusing a new attach
    // or claim of the line until then. Every attach and claim drains first, so that refusal
    // lasts only while another core is inside a dispatch.
    bool irq_attach(int irq, IrqHandler handler, void* arg);
    void irq_detach(int irq);

    // Claim an unused line with AUTH_IRQ and install a full-rights CAP_IRQ.
    // Leave it masked until the server's first wait or ack. Return 0 with out_cap,
    // or -KOS_E*: ENOMEM for pool exhaustion, EMFILE for a full cap table,
    // EOVERFLOW for the task budget, EBUSY for an occupied or retiring line.
    int irq_claim(Thread* c, int line, unsigned int flags, uint32_t* out_cap);

    // Attach this line to the notification `notify_cap` names, as one signaller among
    // others: the ISR raises that capability's badge bit there. Requires CAP_WAIT on the
    // line and CAP_SIGNAL on the notification, and copies the badge into the binding, since
    // ISR context may resolve no capability. Takes a reference on the object.
    // One-way and once only: 0, -KOS_EALREADY (this line already signals something),
    // -KOS_EBADF, -KOS_EPERM (a missing right, or the notification already carries a line
    // claimed on another core) or -KOS_EOVERFLOW (the object's reference count is at its
    // ceiling). A released line leaves the chain, so a re-claim is a new binding and binds
    // afresh.
    int irq_bind_notify(Thread* c, uint32_t irq_cap, uint32_t notify_cap);

    // Rearm every chained signaller of `n` whose badge is in `mask` and which owes one, and
    // flag the signallers of `taken` to be rearmed next time round. Caller holds IrqLock.
    void irq_signallers_rearm(Notification* n, uint32_t mask);
    void irq_signallers_owe_rearm(Notification* n, uint32_t taken);

    // Take binding `index` off its notification's signaller chain, so no later rearm reaches
    // a line that is going away. The binding keeps its object pointer and its reference: a
    // dispatch on another core may already have loaded that pointer, and the reference is
    // what keeps the object alive until it can no longer be reached. Caller holds IrqLock.
    void irq_unchain_signaller(int index);

    // Clear that pointer and drop that reference, at the point the slot returns to the pool
    // and no dispatch can still hold its address. A no-op for a binding that was never
    // attached. Caller holds IrqLock.
    void irq_detach_notify(int index);

#if KICKOS_KERNEL_CORES > 1
    // Refuse a wait on `n` by a caller whose own mask is not exactly the core its lines were
    // claimed on: -KOS_EPERM, never a clamp; 0 where no line signals it. Caller holds the
    // IrqLock the wait runs under.
    int irq_admit_signallers(Thread const* c, Notification const* n);
#endif

    // Unmask the previously-consumed line so it can fire again; 0, or -KOS_E*.
    // Optional and idempotent: the next notify_wait rearms anyway, and a redundant
    // ack after that wait is a no-op (needs_rearm already false). Needs CAP_WAIT, and
    // -KOS_EPERM for a caller whose own mask excludes the line's claim core, as for discard.
    // -KOS_EINVAL for a line that signals no notification: arming it would open a source
    // whose raise has nowhere to land, which is a lost interrupt and not a masked one.
    int irq_ack(Thread* c, uint32_t cap_handle);
    // Discard whatever the controller has latched for this line, right now; 0, or
    // -KOS_E*. Needs CAP_WAIT. The only way an EDGE driver can drop a pending it
    // knows is stale: rearm deliberately preserves an EDGE latch (the coalesce contract),
    // and the controller sits in arch_reserved_blocks, so no grant reaches the register.
    // Leaves the line masked; the intended shape is wait, read the device, discard, ack.
    int irq_discard(Thread* c, uint32_t cap_handle);

    // Drop one reference to IRQ binding `obj_handle`; release the line and free the
    // slot at refs -> 0. Caller holds IrqLock. Above one kernel core the line is released
    // here and the slot returns to the pool from a later reclamation, once no dispatch can
    // still hold its address.
    void irq_ref_drop(int obj_handle, bool teardown);
}

#endif
