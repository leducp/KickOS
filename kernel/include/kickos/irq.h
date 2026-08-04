// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Interrupts as events that wake a thread. Two tiers:
//   Tier 2 (privileged, in-kernel): irq_attach binds a direct handler that runs
//     in ISR context and typically posts a sem/flag.
//   Tier 1 (unprivileged userspace driver): irq_claim/wait/ack/notify. A CAP_IRQ
//     cap names the binding; the generic first-level ISR masks the line and posts
//     it, the driver waits in thread context, services, and acks (unmask).
//     Minting takes AUTH_IRQ; using a claimed line takes possession of the cap.
//     See docs/design-m4.6-irq-driver.md section 2.

#ifndef KICKOS_IRQ_H
#define KICKOS_IRQ_H

#include <stdint.h>

#include <kickos/sync.h>

namespace kickos
{
    struct Thread; // kickos/thread.h: tier-1 claim/wait/ack act on a caller's cap table

    using IrqHandler = void (*)(void* arg);

    // Line -> handler dispatch entry; the ISR reads it by index, never a search.
    struct IrqEntry
    {
        IrqHandler handler = nullptr;
        void* arg = nullptr;
    };

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
    struct IrqBinding
    {
        Semaphore sem;
        int line = -1;
        // Set ONLY when an irq_wait returns (event consumed, line masked by the ISR,
        // awaiting rearm), NEVER in the ISR. Setting it in the ISR races the
        // ack;compute;wait shape: unmasking before the event is serviced re-fires the
        // still-asserted line -> phantom sem post -> next wait returns with no event ->
        // the driver drains an empty device FIFO. Thread context only, under IrqLock.
        // Starts TRUE, so that a claim leaves the line masked and the first wait arms it:
        // there is no window in which the line is armed and unowned.
        bool needs_rearm = true;
        uint8_t trigger = IRQ_EDGE;
        // False until the first arm. That arm discards a latch left from before the
        // line had an owner, whatever the trigger type; a LEVEL binding then keeps
        // discarding on every rearm.
        bool armed_once = false;
    };

    // Seed the dispatch table with the null-object default (call once at boot,
    // before any attach/register).
    void irq_init();
    // Count of IRQs that fired on a line with no driver (masked by the default
    // handler). Best-effort diagnostic, readable outside ISR context.
    uint32_t irq_spurious_count();

    // Tier 2: privileged in-kernel direct handler. Returns false if the line is
    // out of range or already bound (one driver per line); true on success.
    bool irq_attach(int irq, IrqHandler handler, void* arg);
    void irq_detach(int irq);

    // Tier 1: IRQ-as-event (usable from unprivileged userspace via syscalls). The
    // caller must hold AUTH_IRQ; claims `line` (one owner, no stealing), allocates a
    // binding, and installs a full-rights CAP_IRQ into `c`'s table. The line is left
    // MASKED with needs_rearm set, so the first irq_wait arms it in the thread that
    // will consume the event. -> 0 with the cap in *out_cap, or -KOS_E*: the two
    // exhaustion cases are distinct, -KOS_ENOMEM for the binding pool and -KOS_EMFILE
    // for the caller's own table.
    int irq_claim(Thread* c, int line, unsigned int flags, uint32_t* out_cap);
    // Block until the line fires; 0, or -KOS_E*. Auto-rearms the previously-consumed line
    // on entry, so `wait; service` alone keeps receiving IRQs and an explicit irq_ack is
    // OPTIONAL. Needs CAP_WAIT on the cap.
    //
    // -KOS_ECANCELED means the caller was cancelled (thread_kill): the wait was abandoned
    // and the line NOT rearmed, and every later irq_wait answers the same. The one
    // cancellation point in the kernel, and the only park a third party may end.
    int irq_wait(Thread* c, uint32_t cap_handle);
    // Is `t` parked inside irq_wait right now? The predicate thread_kill needs before it
    // may deliver a wait_result to a parked thread. Caller holds IrqLock.
    bool irq_thread_parked(Thread const* t);
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
    // Software-post the binding's notification WITHOUT touching the controller: the
    // TX doorbell a service thread rings so the IRQ thread (sole owner of every
    // peripheral register) primes a transfer. Needs CAP_SIGNAL. Distinct from
    // arch_irq_inject, which raises AT the controller and simulates a device.
    int irq_notify(Thread* c, uint32_t cap_handle);

    // Drop one reference to IRQ binding `obj_handle`; release the line and free the
    // slot at refs -> 0. Called only by the cap layer's accounting.
    void irq_ref_drop(int obj_handle, bool teardown);
}

#endif
