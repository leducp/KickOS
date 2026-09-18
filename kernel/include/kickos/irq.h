// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// IRQ delivery has two interfaces:
// - Kernel handlers run directly in ISR context through irq_attach.
// - Userspace drivers hold CAP_IRQ capabilities and bind delivery to one thread.
//   The ISR masks the line and posts a notification. The driver services the
//   device, then rearms through wait or ack.
// Claiming a line requires AUTH_IRQ; using it requires capability rights.

#ifndef KICKOS_IRQ_H
#define KICKOS_IRQ_H

#include <stdint.h>

#include <kickos/config.h>

#include <kickos/sync.h>

#if KICKOS_KERNEL_CORES > 1
#include <kickos/sys/atomic.h>
#endif

namespace kickos
{
    struct Thread; // kickos/thread.h: tier-1 claim/wait/ack act on a caller's cap table

    using IrqHandler = void (*)(void* arg);

#if KICKOS_KERNEL_CORES > 1
    // The callback pair a dispatch runs for one line.
    struct IrqDispatch
    {
        IrqHandler handler = nullptr;
        void* arg = nullptr;
    };

    // Line -> handler dispatch entry; the ISR reads it by index, never a search. The word
    // names the publication record holding the whole pair. Index 0 names no record and stands
    // for the line's own null-object default, whose argument is the line the dispatch was
    // entered for.
    struct IrqEntry
    {
        Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE> pub{0u};
    };

    // The pair a dispatch entered for `line` would run.
    IrqDispatch irq_published(int line);
#else
    // Line -> handler dispatch entry; the ISR reads it by index, never a search.
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

    // Tier 1 binding: a line plus the notification the driver waits on. LATENCY INVARIANT:
    // the ISR is handed this binding directly as its arg, so it never searches a table in ISR
    // context. It lives in a SlotPool, whose slot addresses are stable for the slot's life,
    // which is what keeps that invariant true once the binding becomes freeable.
    // A freed slot keeps its last contents (slotpool.h), so no field below means anything
    // until irq_claim has seated all five; none of them is a live default. Whether a LINE
    // is held is not stored here: the line's dispatch entry names the null-object default
    // exactly while the line is free.
    struct IrqBinding
    {
        // Bound server thread. Events latch in pending while no server is bound.
        Thread* notify_target = nullptr;
        int line = 0;
        // Bit in the server's notify_pending word, equal to this binding's pool index.
        uint8_t notify_bit = 0;
        // Event pending while notify_target is null.
        bool pending = false;
        // The current wait accepts this line and may be woken by it.
        // Set by irq_notify_wait_enter and cleared by irq_notify_wait_leave under IrqLock.
        bool notify_wake = false;
        // Set when a wait consumes the event, under IrqLock. Never set in the ISR:
        // that would allow ack to unmask a level interrupt before the device is serviced.
        bool needs_rearm = false;
        uint8_t trigger = IRQ_EDGE;
        // False until the first arm. That arm discards a latch left from before the
        // line had an owner, whatever the trigger type; a LEVEL binding then keeps
        // discarding on every rearm.
        bool armed_once = false;
    };

    static_assert(KICKOS_MAX_IRQ_HANDLES <= 32,
                  "a binding's pool index IS its bit in Thread::notify_pending, so a pool "
                  "wider than the word would alias two lines onto one bit");

    // Seed the dispatch table with the null-object default (call once at boot,
    // before any attach/register).
    void irq_init();
    // Count of IRQs that fired on a line with no driver (masked by the default
    // handler). Best-effort diagnostic, readable outside ISR context.
    uint32_t irq_spurious_count();

    // Tier 2: privileged in-kernel direct handler. Returns false if the line is
    // out of range or already bound (one driver per line); true on success.
    //
    // detach leaves the line MASKED and always succeeds. Above one kernel core it does NOT free
    // the line: the line names its retiring record until no dispatch can still hold that record,
    // and an attach or claim of the line is refused until then. Every attach and claim drains
    // first, so that refusal lasts only while another core is inside a dispatch.
    bool irq_attach(int irq, IrqHandler handler, void* arg);
    void irq_detach(int irq);

    // Claim an unused line with AUTH_IRQ and install a full-rights CAP_IRQ.
    // Leave it masked until the server's first wait or ack. Return 0 with out_cap,
    // or -KOS_E*: ENOMEM for pool exhaustion, EMFILE for a full cap table,
    // EOVERFLOW for the task budget, EBUSY for an occupied or retiring line.
    int irq_claim(Thread* c, int line, unsigned int flags, uint32_t* out_cap);
    // Bind delivery to the calling thread and return its one-bit notification mask.
    // Requires CAP_WAIT. The server must bind before waiting. Pending events are
    // transferred to it. Rebinding the same thread succeeds; another gets -KOS_EBUSY.
    int irq_notify_bind(Thread* c, uint32_t cap_handle, uint32_t* out_mask);

    // Release c's binding and preserve unconsumed events for the next server.
    // Does nothing if c is not bound to obj_handle. Caller holds IrqLock.
    void irq_notify_release(Thread* c, int obj_handle);

    // Restrict mask to lines served by c, rearm consumed lines, and allow them
    // to wake this wait. Returns the accepted mask for irq_notify_wait_leave.
    // Caller holds IrqLock.
    uint32_t irq_notify_wait_enter(Thread* c, uint32_t mask);

    // End the wait and return consumed bits from opened, leaving other bits pending.
    // Consumed lines need rearming on the next wait or ack. Caller holds IrqLock.
    uint32_t irq_notify_wait_leave(Thread* c, uint32_t opened);

    // Wait for this line; return 0 or -KOS_E*. Requires CAP_WAIT and a binding
    // to the caller. Rearms on entry, so an explicit ack is optional.
    // Cancellation returns -KOS_ECANCELED on this and all later waits.
    // Teardown releases the binding; cancellation does not rearm the line.
    int irq_wait(Thread* c, uint32_t cap_handle);
    // irq_wait with a relative timeout in microseconds; KOS_TIMEOUT_NONE waits forever.
    // Returns -KOS_ETIMEDOUT on expiry without changing the line's rearm state.
    // Pending controller events remain deliverable.
    int irq_wait_timed(Thread* c, uint32_t cap_handle, uint32_t timeout_us);
    // Unmask the previously-consumed line so it can fire again; 0, or -KOS_E*.
    // OPTIONAL and idempotent: the next irq_wait rearms anyway, and a redundant
    // ack after that wait is a no-op (needs_rearm already false). Needs CAP_WAIT.
    int irq_ack(Thread* c, uint32_t cap_handle);
    // Discard whatever the controller has latched for this line, right now; 0, or
    // -KOS_E*. Needs CAP_WAIT. The ONLY way an EDGE driver can drop a pending it
    // knows is stale: rearm deliberately preserves an EDGE latch (the coalesce contract),
    // and the controller sits in arch_reserved_blocks, so no grant reaches the register.
    // Does NOT unmask; the intended shape is wait; read the device; discard; ack.
    int irq_discard(Thread* c, uint32_t cap_handle);
    // Post a software notification without accessing the controller. Requires CAP_SIGNAL.
    // Returns -KOS_EALREADY if the bit was set; the existing notification remains pending.
    int irq_notify(Thread* c, uint32_t cap_handle);

    // Drop one reference to IRQ binding `obj_handle`; release the line and free the
    // slot at refs -> 0. Caller holds IrqLock. Above one kernel core the line is released
    // here and the slot returns to the pool from a later reclamation, once no dispatch can
    // still hold its address.
    void irq_ref_drop(int obj_handle, bool teardown);
}

#endif
