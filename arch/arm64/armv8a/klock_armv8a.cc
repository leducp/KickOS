// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The cross-core doorbell and the kernel lock coupled to it, on armv8a: the GIC's clear and
// raise, the service body's context synchronization event, the ticket draw's exclusive pair,
// and the secondary park. The protocol over them is in arch/common/doorbell_protocol.cc.
//
// Neither GIC version reports that a target has serviced a software-generated interrupt, and
// GICv2's per-source pending registers are banked to the accessing core: the controller
// carries the wake, and the answer travels in the shared cells.
//
// The lock is a ticket pair: a draw takes g_next_ticket with LDXR/STXR, and the holder alone
// advances g_now_serving with a plain load and an STLR. Both counters are written only inside
// this file.

#include <kickos/arch/arch.h>

#include "../common/gic.h"

#include <kickos/arch/doorbell_protocol.h>
#include <kickos/arch/klock_owner.h>

#include <kickos/sys/atomic.h>

#include <stddef.h>
#include <stdint.h>

// The doorbell half is built whenever something rings it: a shared kernel's peers above one
// core, an AMP node's peer nodes at one core. The lock and the park below stay above one core,
// being about cores of ONE kernel.
#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)

extern "C"
{
    void kfault_terminate(void) __attribute__((noreturn));
#if KICKOS_BENCH && KICKOS_KERNEL_CORES > 1
    // Declared rather than included: this TU is below <kickos/bench.h>.
    void kickos_bench_lock_draw(uint32_t retries, uint32_t queued);
#endif
}

using kickos::doorbell::g_answer;
using kickos::doorbell::g_request;

namespace
{
#if defined(KICKOS_ENABLE_SELFTEST)
    // Per-core doorbell services, read across nodes, so placed with the cells.
    KICKOS_AMP_SHARED("cells.served")
    kickos::doorbell::PartCell g_served[KICKOS_DOORBELL_CORES] = {};
#if KICKOS_NUM_CORES > 1
    // Per-core instruction-side rendezvous initiated. No peer reads it.
    kickos::doorbell::PartCell g_initiated[KICKOS_NUM_CORES] = {};
#endif
#endif

#if KICKOS_KERNEL_CORES > 1
    // Separate lines: every draw takes g_next_ticket's line exclusive in the inner-shareable
    // domain, which on a shared line would invalidate it under every waiter loading
    // g_now_serving. DDI 0487 M.b B2.12.5 (page B2-340) asks for a reservation granule between
    // objects reached by exclusive accesses.
    alignas(KICKOS_DOORBELL_LINE) uint32_t g_next_ticket = 0;
    alignas(KICKOS_DOORBELL_LINE) uint32_t g_now_serving = 0;

    // The acquire half of the lock: it pairs with the STLR the holder releases with.
    uint32_t now_serving(void)
    {
        uint32_t v = 0;
        __asm volatile("ldar %w0, [%1]" : "=r"(v) : "r"(&g_now_serving) : "memory");
        return v;
    }

    // LDXR, not LDAXR: a drawn ticket grants nothing and so owes no acquire.
    //
    // No call may enter the retry interval: DDI 0487 M.b B2.12.5 (page B2-340), condition 2,
    // forbids any direct or indirect System register write, address translation instruction,
    // cache or TLB maintenance instruction, exception generating instruction, exception return,
    // indirect branch or Branch with Link between a Store-Exclusive returning a failing result
    // and the retry of the corresponding Load-Exclusive, or a PE without FEAT_LSE loses its
    // forward-progress guarantee. The bench arm's counter is a plain ADD, which that list does
    // not name.
#if KICKOS_BENCH
    uint32_t kernel_lock_draw(uint32_t& retries)
    {
        uint32_t tick = 0;
        uint32_t want = 0;
        uint32_t res = 0;
        uint32_t tries = 0;
        __asm volatile("1:     add     %w3, %w3, #1\n"
                       "       ldxr    %w0, [%4]\n"
                       "       add     %w1, %w0, #1\n"
                       "       stxr    %w2, %w1, [%4]\n"
                       "       cbnz    %w2, 1b\n"
                       : "=&r"(tick), "=&r"(want), "=&r"(res), "+r"(tries)
                       : "r"(&g_next_ticket)
                       : "memory");
        retries = tries - 1u;
        return tick;
    }
#else
    uint32_t kernel_lock_draw(void)
    {
        uint32_t tick = 0;
        uint32_t want = 0;
        uint32_t res = 0;
        __asm volatile("1:     ldxr    %w0, [%3]\n"
                       "       add     %w1, %w0, #1\n"
                       "       stxr    %w2, %w1, [%3]\n"
                       "       cbnz    %w2, 1b\n"
                       : "=&r"(tick), "=&r"(want), "=&r"(res)
                       : "r"(&g_next_ticket)
                       : "memory");
        return tick;
    }
#endif
#endif
}

extern "C"
{

// The far side of the doorbell, on the calling core. Reached from the SGI handler and from a
// poll inside a spin, and MASKED either way.
void kickos_arm64_doorbell_service(void)
{
    uint32_t const me = arch_doorbell_core();
    // Observed once and answered from the observation, never re-read: the ISB below must
    // attest to THIS snapshot, and a request raised after it is not one it covers.
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

    // A context synchronization event on this PE: until a PE takes one, instructions it has
    // already fetched may be re-executed with no bound (DDI 0487 M.b section B2.7.4.2), and no
    // operation makes one PE synchronize another (Glossary, "Context Synchronization event").
    // ISB flushes the pipeline in the PE and is such an event (section C6.2.177); it is spelled
    // explicitly because the poll runs this body from inside a spin, which enters no exception,
    // and whether exception entry is itself such an event rests on FEAT_ExS and SCTLR_EL1.EIS.
    // Placed after the snapshot and before the answer stores, so an initiator that has seen an
    // answer has seen this.
    __asm volatile("isb" ::: "memory");
#if defined(KICKOS_ENABLE_SELFTEST)
    g_served[me].v = g_served[me].v.load() + 1u;
#endif
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

// Runs the service body with this core's interrupts masked: the body is not re-entrant
// against itself, an answer write preempted between its read and its store publishing a
// stale sequence that an initiator waits on forever.
void kickos_doorbell_poll(void)
{
    if (not kickos::doorbell::pending())
    {
        return;
    }
    arch_irq_state_t const state = arch_irq_save();
    // Before the service: a raise landing after the clear stays pending and is delivered.
    kickos_armv8a_gic_doorbell_clear();
    kickos_arm64_doorbell_service();
#if KICKOS_KERNEL_CORES > 1
    // After the clear that absorbed it: the clear above drops every source's pending bit, a
    // reschedule among them, and the cell is what says one was owed.
    if (kickos_kernel_core_resched_owed() != 0)
    {
        arch_ipi_resched_self();
    }
#endif
    arch_irq_restore(state);
}

void kickos_doorbell_raise(uint32_t cores)
{
    kickos_armv8a_gic_doorbell_send(cores);
}

#if KICKOS_KERNEL_CORES > 1
// GICD_SGIR reaches the sending core like any other target, and the pending state is one bit,
// so a raise over one already pending is idempotent.
void arch_ipi_resched_self(void)
{
    kickos_armv8a_gic_doorbell_send(1u << arch_cpu_id());
}
#endif

// A full barrier, and neither half of an acquire/release pair: the pairing it serves is a store
// then a load on both sides, which is the one direction release and acquire leave free.
// Reached through a plain call that no callgraph gate follows, so its body is asserted out of
// the linked image (tests/static/check_ipi_fence.sh).
void arch_ipi_fence(void)
{
    __asm volatile("dmb ish" ::: "memory");
}

// One poke and one wait over `peers`, whose whole effect is the ISB every serviced core runs.
#if KICKOS_NUM_CORES > 1
void kickos_arm64_instruction_side_rendezvous(uint32_t peers)
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
#endif

#if defined(KICKOS_ENABLE_SELFTEST)
uint32_t arch_ipi_deferred(uint32_t core)
{
    return kickos_armv8a_gic_deferred(core);
}

uint32_t arch_ipi_seat_set(uint32_t core, uint32_t seated)
{
    return kickos_armv8a_gic_seat_set(core, seated);
}

// Services in the low half, rendezvous initiated in the high half (arch.h, arch_ipi_counts).
uint64_t arch_ipi_counts(uint32_t core)
{
    if (core >= KICKOS_DOORBELL_CORES)
    {
        return 0;
    }
    uint64_t initiated = 0;
#if KICKOS_NUM_CORES > 1
    initiated = g_initiated[core].v.load();
#endif
    return (initiated << 32) | static_cast<uint64_t>(g_served[core].v.load());
}
#endif

#if KICKOS_KERNEL_CORES > 1
// The poll in this loop is what keeps the coupling sound: a caller acquires with interrupts
// masked, so a raise aimed at this core is pending and undeliverable while an initiator holding
// the lock waits on it.
//
// The turn test precedes the poll: when the turn has come the previous holder has released, so
// no lock-holding initiator can be waiting on this core's answer and the skipped poll strands
// nobody. The draw itself carries no poll either, so a core observed spinning answers only once
// it has finished drawing.
void arch_kernel_lock(void)
{
#if KICKOS_BENCH
    uint32_t retries = 0;
    uint32_t const ticket = kernel_lock_draw(retries);
    kickos_bench_lock_draw(retries, ticket - now_serving());
#else
    uint32_t const ticket = kernel_lock_draw();
#endif
    while (true)
    {
        if (now_serving() == ticket)
        {
            kickos::klock::owner_take();
            return;
        }
        kickos_doorbell_poll();
        kickos::doorbell::part_spin();
    }
}

// STLR pairs with the LDAR the wait reads the turn with, so everything done under the lock
// publishes before the next ticket is served.
void arch_kernel_unlock(void)
{
    kickos::klock::owner_drop();
    uint32_t v = 0;
    __asm volatile("ldr  %w0, [%1]\n"
                   "add  %w0, %w0, #1\n"
                   "stlr %w0, [%1]"
                   : "=&r"(v)
                   : "r"(&g_now_serving)
                   : "memory");
}
#endif

// Where a core with no thread to run waits for a doorbell. Its interface and its vectors are
// already live, so a raise arrives as an ordinary interrupt.
//
// g_contend picks which state the check wants this core in, and the two contending states are
// what make each half of the coupling checkable: interrupts open, so the vector is what answers,
// or the kernel lock taken under this core's own mask, where only the poll in the acquire loop
// can. The spinning cell is what tells the initiator this core is in the second.
//
// A shared kernel leaves this loop for good once it has published a thread for this core.
#if KICKOS_NUM_CORES > 1
void kickos_armv8a_doorbell_park(void)
{
    using namespace kickos::doorbell;

    // This core's own interface enabled the timer PPI, so masking its bank down to the doorbell
    // is what keeps kickos_isr_timer out of a core that reaches no scheduler.
    kickos_armv8a_gic_doorbell_only();
#if KICKOS_AMP_NODE
    // What was sent to this core before it could be poked: a sender that found it unseatable
    // published anyway and skipped the raise. Read once here, behind the barrier pairing with
    // the sender's, before this core waits for a doorbell it may already have missed. Placed
    // ahead of the unmask: the service body's only exclusion is this core's mask and the vector
    // reaches the same body, so a doorbell landing inside this call would re-enter take_call or
    // release_call. An SGI raised while it runs stays pending, so waiting misses nothing.
    arch_ipi_fence();
    kickos_amp_node_service();
#endif
    __asm volatile("msr daifclr, #2" ::: "memory");
    uint32_t const me = arch_cpu_id();
    while (true)
    {
#if KICKOS_KERNEL_CORES > 1
        if (kickos_kernel_core_seated() != 0 and kickos_kernel_core_ready() != 0)
        {
            // Masked and never restored: the scheduler's first switch erets onto a frame
            // carrying its own interrupt state. percore_init restores the bank doorbell_only
            // narrowed and drops this core's doorbell pending state, so it belongs on this arm
            // alone.
            (void)arch_irq_save();
            kickos_armv8a_gic_percore_init();
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
            __asm volatile("yield" ::: "memory");
            continue;
        }
        arch_irq_state_t const state = arch_irq_save();
        // Set after the mask and cleared before its restore, so the flag is never set with this
        // core's interrupts open: that is the whole content of what a reader concludes from it.
        g_spinning[me].v = 1u;
        arch_kernel_lock();
        g_held[me].v = g_held[me].v.load() + 1u;
        arch_kernel_unlock();
        g_spinning[me].v = 0u;
        arch_irq_restore(state);
        __asm volatile("yield" ::: "memory");
    }
}
#endif

}

#endif
