// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What the cross-core doorbell and the kernel lock coupled to it cost on rv64imac: the CLINT's
// raise, sip.SSIP's clear and self-raise, the service body's translation fence, the ticket
// draw's AMO, and the secondary park. The protocol over them is in
// arch/common/doorbell_protocol.cc.
//
// The rendezvous is shared memory: a CLINT msip word carries the wake and reports nothing back,
// so the answer travels in the shared cells. The lock is a ticket pair: a draw takes
// g_next_ticket with AMOADD.W, and the holder alone advances g_now_serving with a plain load
// and a plain store under a fence. Both counters are written only inside this file.
//
// SSIP is double-booked on this arch and the dispatch is where that is resolved: the same cause
// carries a peer's raise and this hart's own device-line injection, so the cell and not the
// raise is what says a service is owed (kickos_rv64_doorbell_pending).

#include <kickos/arch/arch.h>
#include <kickos/arch/percpu.h>
#include <kickos/arch/doorbell_protocol.h>
#include <kickos/arch/klock_owner.h>
#include <kickos/arch/rv64_doorbell.h>

#if KICKOS_NUM_CORES > 1

#include <kickos/sys/atomic.h>

#include <stddef.h>
#include <stdint.h>

extern "C" void kfault_terminate(void) __attribute__((noreturn));
extern "C" void kickos_rv64_init(void);
#if KICKOS_BENCH && KICKOS_KERNEL_CORES > 1
// Declared rather than included: this TU is below <kickos/bench.h>.
extern "C" void kickos_bench_lock_draw(uint32_t retries, uint32_t queued);
#endif

using kickos::doorbell::g_answer;
using kickos::doorbell::g_request;

namespace
{
#if defined(KICKOS_ENABLE_SELFTEST)
    // Per-core doorbell services, and per-core instruction-side rendezvous initiated. Only
    // the first is read across nodes, so only it is placed in the shared region.
    KICKOS_AMP_SHARED("cells.served")
    kickos::doorbell::PartCell g_served[KICKOS_DOORBELL_CORES] = {};
    kickos::doorbell::PartCell g_initiated[KICKOS_NUM_CORES] = {};
#endif

    // g_online[i]: nonzero once hart i has reached the park. Written by that hart alone.
    kickos::doorbell::PartCell g_online[KICKOS_NUM_CORES] = {};

    // sie.STIE, which a parked secondary drops: it has armed no deadline and reaches no
    // scheduler, so a timer trap there would enter kickos_isr_timer with no core to serve.
    constexpr uint64_t SIE_STIE = 1ull << 5;

    // sip.SSIP, which supervisor mode owns and may write.
    constexpr uint64_t SIP_SSIP = 1ull << 1;

    // Drops the doorbell's pending state on the CALLING hart. A hart servicing the doorbell
    // outside its handler owes this call.
    void doorbell_clear(void)
    {
        __asm volatile("csrc sip, %0" ::"r"(SIP_SSIP) : "memory");
    }

    // Raises it again on this hart. sip.SSIP is supervisor-owned, so a self-raise needs no trip
    // through the machine-mode trampoline a peer's raise takes.
    void doorbell_raise_self(void)
    {
        __asm volatile("csrs sip, %0" ::"r"(SIP_SSIP) : "memory");
    }

#if KICKOS_KERNEL_CORES > 1
    // Separate lines so a draw's write does not invalidate the line every waiter is loading
    // g_now_serving from. The width above is a choice and not a measurement
    // (kickos/arch/doorbell_part.h), and no reservation granule is at stake here: the ticket
    // takes LR/SC out of the lock entirely.
    alignas(KICKOS_DOORBELL_LINE) uint32_t g_next_ticket = 0;
    alignas(KICKOS_DOORBELL_LINE) uint32_t g_now_serving = 0;

    // Relaxed, with no ordering bits: a drawn ticket grants nothing and publishes nothing.
    // zaamo is in this board's -march string (arch/riscv/chip/virt_rv64/cpu.cmake).
    uint32_t kernel_lock_draw(void)
    {
        uint32_t tick = 0;
        __asm volatile("amoadd.w %0, %1, (%2)"
                       : "=r"(tick)
                       : "r"(1u), "r"(&g_next_ticket)
                       : "memory");
        return tick;
    }

    uint32_t now_serving(void)
    {
        uint32_t v = 0;
        __asm volatile("lw %0, 0(%1)" : "=r"(v) : "r"(&g_now_serving) : "memory");
        return v;
    }
#endif
}

extern "C"
{

int kickos_rv64_doorbell_pending(void)
{
    if (kickos::doorbell::pending())
    {
        return 1;
    }
    return 0;
}

// The far side of the doorbell, on the calling hart. Reached from the supervisor dispatch and
// from a poll inside a spin, and MASKED either way.
void kickos_rv64_doorbell_service(void)
{
    uint32_t const me = arch_doorbell_core();

    // The order is the whole contract and it has three parts: observe the request, then fence,
    // then answer. An initiator writes the tables, raises the request, and waits on the answer,
    // so only having observed the request does this hart's fence sit after those table writes.
    // A fence executed before the request is loaded attests to nothing the initiator cares
    // about: the peer could fence, the initiator could then write tables and raise, and the
    // peer could answer a request it never fenced for, leaving the initiator free to release
    // memory this hart still holds translations for.
    //
    // Seq is acquire on load and release on store, which is what stops the compiler and the
    // machine from undoing the order: the acquire pairs with the initiator's release of the
    // request, and the release publishes the fence ahead of the answer.
    uint32_t asked[KICKOS_DOORBELL_CORES] = {};
    bool owed = false;
    for (uint32_t from = 0; from < KICKOS_DOORBELL_CORES; from++)
    {
        asked[from] = g_request[from].seq[me].load();
        if (asked[from] != g_answer[me].seq[from].load())
        {
            owed = true;
        }
    }
    if (not owed)
    {
        return;
    }

    // One fence per service, not one per requester: SFENCE.VMA with rs1 = rs2 = x0 is global,
    // so a single execution after observing any outstanding request covers every one of them.
    //
    // The translation half a peer owes for itself: SFENCE.VMA orders this hart's address
    // translation against another hart's table writes, and the ISA gives no operation by which
    // one hart performs it for another (Privileged ISA, "Supervisor Memory-Management Fence").
    //
    // The instruction half is not covered here: its operation is FENCE.I, and Zifencei is not
    // in this board's ISA baseline (arch/riscv/chip/virt_rv64/cpu.cmake).
    __asm volatile("sfence.vma zero, zero" ::: "memory");

#if defined(KICKOS_ENABLE_SELFTEST)
    g_served[me].v = g_served[me].v.load() + 1u;
#endif

    // The sequence observed above, never a re-read: a request raised after the fence is not one
    // this fence covers, and answering it here would attest to a fence that never saw it.
#if KICKOS_KERNEL_CORES > 1
    // After the snapshot above and before the answer stores below; both halves are the
    // contract (kernel/irq/irq_route.cc, line_op_ask).
    kickos_irq_route_service();
#endif

    for (uint32_t from = 0; from < KICKOS_DOORBELL_CORES; from++)
    {
        if (asked[from] != g_answer[me].seq[from].load())
        {
            g_answer[me].seq[from] = asked[from];
        }
    }
#if KICKOS_AMP_NODE
    // After the answers, which is the contract: an AMP payload drain may not delay the
    // rendezvous a shared kernel's callers wait on through this same body. The early return
    // above cannot lose a payload wake, a send raising the request cell like any other.
    kickos_amp_node_service();
#endif
}

// Runs the service body with this hart's interrupts masked: the body is not re-entrant
// against itself, an answer write preempted between its read and its store publishing a
// stale sequence that an initiator waits on forever.
void kickos_doorbell_poll(void)
{
    if (kickos_rv64_doorbell_pending() == 0)
    {
        return;
    }
    arch_irq_state_t const state = arch_irq_save();
    // Before the service: a raise landing after the clear stays pending and is delivered.
    doorbell_clear();
    kickos_rv64_doorbell_service();

    // The clear above dropped the one cause every raise arrives on, and this body services
    // exactly one of the three that ride it. Whatever the cells still say is owed is raised
    // again here, or it is lost: a device line whose raise this poll absorbed would leave
    // its driver asleep for good, which is a hang and not a failed assertion.
    bool owed = kickos_rv64_inject_owed() != 0;
#if KICKOS_KERNEL_CORES > 1
    if (kickos_kernel_core_resched_owed() != 0)
    {
        owed = true;
    }
#endif
    if (owed)
    {
        doorbell_raise_self();
    }
    arch_irq_restore(state);
}

void kickos_doorbell_raise(uint32_t cores)
{
    kickos_rv64_doorbell_send(cores);
}

#if KICKOS_KERNEL_CORES > 1
// sip.SSIP is supervisor-owned, so this hart raises its own doorbell directly rather than
// through the CLINT and the machine-mode trampoline a peer's raise needs.
void arch_ipi_resched_self(void)
{
    doorbell_raise_self();
}
#endif

// A FULL barrier, and neither half of an acquire/release pair: the pairing it serves is a store
// then a load on both sides, which is the one direction release and acquire leave free.
// Reached through a plain call that no callgraph gate follows, so its body is asserted out of
// the linked image (tests/static/check_ipi_fence.sh).
void arch_ipi_fence(void)
{
    __asm volatile("fence rw, rw" ::: "memory");
}

// One poke and one wait over `peers`, whose whole effect is the fence every serviced hart runs.
//
// The translation half only. RISC-V gives no broadcast form of SFENCE.VMA, so a peer holding a
// space whose tables changed must be made to run its own. The instruction half would be FENCE.I,
// absent from this board's ISA baseline, and this call does not stand in for it.
//
// arch_ipi_counts calls its high half instruction-side; on this backend it counts these.
void kickos_rv64_translation_rendezvous(uint32_t peers)
{
#if defined(KICKOS_ENABLE_SELFTEST)
    if (peers != 0)
    {
        uint32_t const me = arch_cpu_id();
        g_initiated[me].v = g_initiated[me].v.load() + 1u;
    }
#endif
    arch_ipi_send(peers);
    arch_ipi_wait(peers);
}

#if defined(KICKOS_ENABLE_SELFTEST)
// Services in the low half, rendezvous initiated in the high half (arch.h, arch_ipi_counts).
uint64_t arch_ipi_counts(uint32_t core)
{
    if (core >= KICKOS_DOORBELL_CORES)
    {
        return 0;
    }
    return (static_cast<uint64_t>(g_initiated[core].v.load()) << 32)
           | static_cast<uint64_t>(g_served[core].v.load());
}
#endif

#if KICKOS_KERNEL_CORES > 1
// The poll in this loop is what keeps the coupling sound: a caller acquires with interrupts
// masked, so a raise aimed at this core is pending and undeliverable while an initiator holding
// the lock waits on it.
//
// The turn test precedes the poll: when the turn has come the previous holder has released, so
// no lock-holding initiator can be waiting on this hart's answer and the skipped poll strands
// nobody. The draw carries no poll either, which here is one instruction wide.
void arch_kernel_lock(void)
{
    uint32_t const ticket = kernel_lock_draw();
#if KICKOS_BENCH
    // amoadd.w cannot retry, so the retry figure is a structural zero on this arch.
    kickos_bench_lock_draw(0u, ticket - now_serving());
#endif
    while (true)
    {
        if (now_serving() == ticket)
        {
            break;
        }
        kickos_doorbell_poll();
        kickos::doorbell::part_spin();
    }
    // The acquire half. This baseline has no Zalasr load-acquire, so it is written as a fence.
    __asm volatile("fence r, rw" ::: "memory");
    kickos::klock::owner_take();
}

// A release store, which the fence plus the plain store is on RISC-V: it pairs with the fence
// the wait leaves on, so everything done under the lock publishes before the next ticket is
// served.
void arch_kernel_unlock(void)
{
    kickos::klock::owner_drop();
    uint32_t v = 0;
    __asm volatile("lw     %0, 0(%1)\n"
                   "addi   %0, %0, 1\n"
                   "fence  rw, w\n"
                   "sw     %0, 0(%1)"
                   : "=&r"(v)
                   : "r"(&g_now_serving)
                   : "memory");
}
#endif

// Where a hart with no thread to run waits for a doorbell. Its vector and its sie are already
// live, so a raise arrives as an ordinary supervisor software interrupt.
//
// g_contend PICKS WHICH STATE THE CHECK WANTS THIS HART IN, and the two contending states are
// what make each half of the coupling checkable: interrupts open, so the vector is what answers,
// or the kernel lock taken under this hart's own mask, where only the poll in the acquire loop
// can. The spinning cell is what tells the initiator this hart is in the second.
//
// A SHARED KERNEL LEAVES THIS LOOP FOR GOOD once it has published a thread for this hart.
void kickos_rv64_doorbell_park(void)
{
    using namespace kickos::doorbell;

    // This hart armed no deadline and reaches no scheduler until it leaves this loop, so its
    // timer stays out: stimecmp's reset value is not architecturally all-ones.
    arch_timer_disarm();
    __asm volatile("csrc sie, %0" ::"r"(SIE_STIE) : "memory");
    __asm volatile("csrsi sstatus, 2" ::: "memory"); // sstatus.SIE
#if KICKOS_KERNEL_CORES > 1
    uint32_t const me = arch_cpu_id();
#endif
    while (true)
    {
#if KICKOS_KERNEL_CORES > 1
        if (kickos_kernel_core_seated() != 0 and kickos_kernel_core_ready() != 0)
        {
            // Masked and never restored: the scheduler's first switch srets onto a frame
            // carrying its own interrupt state. The timer goes back because this hart is about
            // to own a scheduler that arms deadlines.
            (void)arch_irq_save();
            __asm volatile("csrs sie, %0" ::"r"(SIE_STIE) : "memory");
            kickos_kernel_core_arrive();
            kickos_kernel_core_start();
        }
#endif
        uint32_t const contend = g_contend.load();
        if (contend == CONTEND_PARK)
        {
            __asm volatile("wfi");
            continue;
        }
        if (contend == CONTEND_OPEN)
        {
            // No WFI: this arm has to observe the next store, and it takes its raise through
            // the vector rather than through any poll of its own.
            __asm volatile("nop" ::: "memory");
            continue;
        }
#if KICKOS_KERNEL_CORES > 1
        arch_irq_state_t const state = arch_irq_save();
        // Set after the mask and cleared before its restore, so the flag is never set with this
        // hart's interrupts open: that is the whole content of what a reader concludes from it.
        g_spinning[me].v = 1u;
        arch_kernel_lock();
        g_held[me].v = g_held[me].v.load() + 1u;
        arch_kernel_unlock();
        g_spinning[me].v = 0u;
        arch_irq_restore(state);
#endif
        __asm volatile("nop" ::: "memory");
    }
}

// A released secondary's first supervisor code. kickos_rv64_init is entirely per-hart and is
// what seats this hart's identity, so it runs here exactly as it does on the boot hart.
uint32_t kickos_rv64_core_online_read(uint32_t id)
{
    if (id >= KICKOS_NUM_CORES)
    {
        return 0;
    }
    return g_online[id].v.load();
}

void kickos_rv64_secondary_entry(void)
{
    arch_timer_disarm();
    kickos_rv64_init();

    uint32_t const id = arch_cpu_id();
    if (id != 0 and id < KICKOS_NUM_CORES)
    {
        g_online[id].v = 1u;
        kickos_rv64_doorbell_park();
    }
    while (true)
    {
        __asm volatile("wfi");
    }
}

}

#endif
