// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Interrupts as events that wake a thread. Two tiers:
//   Tier 2 (privileged, in-kernel only): irq_attach binds a direct handler that runs
//     in ISR context. No syscall reaches it.
//   Tier 1 (unprivileged userspace driver): irq_claim/attach/wait/ack/notify. A CAP_IRQ
//     cap names the binding; the serving thread binds its notification bit, the generic
//     first-level ISR masks the line and sets that bit, and the driver waits in thread
//     context, services, and acks (unmask).
//     Minting takes AUTH_IRQ; using a claimed line takes possession of the cap.
//     Two threads may share one line with different rights; cap_teardown releases it.

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
        // The thread this line's notification is delivered into, seated by irq_notify_bind
        // and cleared when that thread drops the capability. A raise arriving while it is
        // null latches in `pending` instead, which is what keeps a doorbell rung before the
        // server's first bind.
        Thread* notify_target = nullptr;
        int line = 0;
        // Which bit of the target's notify_pending this line owns: the binding's own pool
        // index, so no two live bindings can collide and nothing has to allocate it.
        uint8_t notify_bit = 0;
        // A raise no notification word has taken. Set only while notify_target is null.
        bool pending = false;
        // Whether the wait notify_target is in right now ACCEPTS this line, which is what
        // lets a raise END that wait rather than only set its bit. Seated under IrqLock by
        // irq_notify_wait_enter and cleared by irq_notify_wait_leave; a wait that does not
        // accept the line leaves the bit standing for one that does.
        bool notify_wake = false;
        // Set ONLY when an irq_wait returns (event consumed, line masked by the ISR,
        // awaiting rearm), NEVER in the ISR. Setting it in the ISR races the
        // ack;compute;wait shape: unmasking before the event is serviced re-fires the
        // still-asserted line -> phantom notification -> next wait returns with no event ->
        // the driver drains an empty device FIFO. Thread context only, under IrqLock.
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

    // Tier 1: IRQ-as-event (usable from unprivileged userspace via syscalls). The
    // caller must hold AUTH_IRQ; claims `line` (one owner, no stealing), allocates a
    // binding, and installs a full-rights CAP_IRQ into `c`'s table. The line is left
    // MASKED with needs_rearm set, so the first irq_wait arms it in the thread that
    // will consume the event. -> 0 with the cap in *out_cap, or -KOS_E*: the exhaustion cases
    // are distinct: -KOS_ENOMEM the binding pool or a publication record, -KOS_EMFILE the
    // caller's own table, -KOS_EOVERFLOW the calling TASK's ceiling with the pool still holding
    // slots (task.h). Above one kernel core -KOS_EBUSY also covers a line whose previous
    // binding is still retiring, which a later claim of the same line takes.
    int irq_claim(Thread* c, int line, unsigned int flags, uint32_t* out_cap);
    // Bind this line's notification into the CALLING thread's word, and answer the one-bit
    // mask it arrives in. Needs CAP_WAIT. The server does this ONCE before its first wait:
    // the claim runs in the spawner, which delegates the capability and closes its own copy,
    // so the claiming thread is not the serving one and cannot stand in for it. Idempotent
    // for the thread already bound, -KOS_EBUSY for a line another thread serves. A raise
    // latched before the bind is drained into the word here.
    int irq_notify_bind(Thread* c, uint32_t cap_handle, uint32_t* out_mask);

    // Drop `c`'s bind on the binding `obj_handle` names. A raise still standing in c's word
    // goes back to the binding's own latch rather than away, so a binding another capability
    // keeps alive still owes it to whoever binds next. A no-op where `c` is not the bound
    // server. Caller holds IrqLock.
    void irq_notify_release(Thread* c, int obj_handle);

    // Enter a wait that ACCEPTS the notification bits in `mask`. Narrows the mask to the lines
    // `c` really serves, so a caller naming a bit of somebody else's binding gets it dropped
    // rather than reaching that binding; rearms each accepted line a previous pass consumed,
    // exactly as a wait on that line alone rearms on entry; and flags each so a raise ends
    // THIS wait. Answers the narrowed mask, which is what the matching leave takes.
    // Caller holds IrqLock.
    uint32_t irq_notify_wait_enter(Thread* c, uint32_t mask);

    // Leave it. Unflags `opened`, takes whichever of those bits stand in c's word, clears
    // them, and flags each taken line for the rearm its next wait entry or its irq_ack will
    // perform. A bit outside `opened` is left standing. Answers the bits taken. Caller holds
    // IrqLock.
    uint32_t irq_notify_wait_leave(Thread* c, uint32_t opened);

    // Block until the line fires; 0, or -KOS_E*. Auto-rearms the previously-consumed line
    // on entry, so `wait; service` alone keeps receiving IRQs and an explicit irq_ack is
    // OPTIONAL. Needs CAP_WAIT on the cap, and the caller must hold the bind.
    //
    // -KOS_ECANCELED means the caller was cancelled (thread_kill): the wait was abandoned
    // and every later irq_wait answers the same. The line is left as the last rearm set it,
    // and the exiting thread's cap drop is what detaches and masks it. The one cancellation
    // point in the kernel: the one primitive that REFUSES to re-block a cancelled caller.
    int irq_wait(Thread* c, uint32_t cap_handle);
    // irq_wait with a deadline of `timeout_us` RELATIVE microseconds, KOS_TIMEOUT_NONE being
    // exactly irq_wait. -KOS_ETIMEDOUT where it passes with no raise; the line is left as the
    // entry rearm set it, so the next wait does not re-arm and a raise already latched at the
    // controller is still delivered.
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
    // Software-post the binding's notification WITHOUT touching the controller: the
    // TX doorbell a service thread rings so the IRQ thread (sole owner of every
    // peripheral register) primes a transfer. Needs CAP_SIGNAL. Distinct from
    // arch_irq_inject, which raises AT the controller and simulates a device.
    // -KOS_EALREADY where the bit was already set: the post is absorbed, as it must be,
    // and this is the only channel a producer has for seeing a server that stopped draining.
    int irq_notify(Thread* c, uint32_t cap_handle);

    // Drop one reference to IRQ binding `obj_handle`; release the line and free the
    // slot at refs -> 0. Caller holds IrqLock. Above one kernel core the line is released
    // here and the slot returns to the pool from a later reclamation, once no dispatch can
    // still hold its address.
    void irq_ref_drop(int obj_handle, bool teardown);
}

#endif
