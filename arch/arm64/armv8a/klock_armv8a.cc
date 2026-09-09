// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What the cross-core doorbell and the kernel lock coupled to it cost on armv8a: the GIC's
// clear and raise, the service body's context synchronization event, the lock word's exclusive
// pair, and the secondary park. The protocol over them is in arch/common/doorbell_protocol.cc.
//
// THE RENDEZVOUS IS SHARED MEMORY. Neither GIC version reports that a target has SERVICED a
// software-generated interrupt, and GICv2's per-source pending registers are banked to the
// accessing core: the controller carries the wake, the answer travels in the shared cells.
//
// The lock word is written only through LDAXR/STXR inside this file.

#include <kickos/arch/arch.h>

#include "../common/gic.h"

#include <kickos/arch/doorbell_protocol.h>

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
    // The lock word: 0 free, 1 held, on a line of its own.
    alignas(KICKOS_DOORBELL_LINE) uint32_t g_kernel_lock = 0;

    // LDAXR/STXR, which is architectural on ARMv8-A over one inner-shareable domain. `res` is 0
    // only on a store that took the word, and the MOV covers the path that never stored.
    // CLREX drops the monitor a taken branch would leave set.
    bool kernel_lock_claim(void)
    {
        uint32_t res = 1u;
        uint32_t cur = 0u;
        __asm volatile("       mov     %w0, #1\n"
                       "       ldaxr   %w1, [%2]\n"
                       "       cbz     %w1, 2f\n"
                       "       clrex\n"
                       "       b       1f\n"
                       "2:     stxr    %w0, %w3, [%2]\n"
                       "1:\n"
                       : "=&r"(res), "=&r"(cur)
                       : "r"(&g_kernel_lock), "r"(1u)
                       : "memory");
        return res == 0u;
    }
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

    // A Context synchronization event on THIS PE: until a PE takes one, instructions it has
    // already fetched may be re-executed with no bound (DDI 0487 M.b section B2.7.4.2), and no
    // operation makes one PE synchronize another (Glossary, "Context Synchronization event").
    // ISB flushes the pipeline in the PE and IS such an event (section C6.2.177).
    //
    // EXPLICIT: the poll runs this body from inside a spin, which enters no exception, and
    // whether exception entry is itself such an event rests on FEAT_ExS and SCTLR_EL1.EIS.
    //
    // After the snapshot and before the answer stores, so an initiator that has seen an
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
    // AFTER THE ANSWERS, and that order is the contract: an AMP payload drain may not delay
    // the rendezvous a shared kernel's callers wait on through this same body. The early
    // return above cannot lose a payload wake, a send raising the request cell like any other.
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
    // BEFORE THE SERVICE: a raise landing after the clear stays pending and is delivered.
    kickos_armv8a_gic_doorbell_clear();
    kickos_arm64_doorbell_service();
#if KICKOS_KERNEL_CORES > 1
    // AFTER THE CLEAR THAT ABSORBED IT: the clear above drops every source's pending bit,
    // a reschedule among them, and the cell is what says one was owed.
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

// A FULL barrier, and neither half of an acquire/release pair: the pairing it serves is a store
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
// THE POLL IN THIS LOOP IS WHAT KEEPS THE COUPLING SOUND: a caller acquires with interrupts
// masked, so a raise aimed at this core is pending and undeliverable while an initiator holding
// the lock waits on it.
void arch_kernel_lock(void)
{
    while (true)
    {
        if (kernel_lock_claim())
        {
            return;
        }
        kickos_doorbell_poll();
        __asm volatile("yield" ::: "memory");
    }
}

// STLR pairs with the LDAXR the claim takes the word with, so everything done under the lock
// publishes before the word reads free.
void arch_kernel_unlock(void)
{
    __asm volatile("stlr wzr, [%0]" ::"r"(&g_kernel_lock) : "memory");
}
#endif

// Where a core with no thread to run waits for a doorbell. Its interface and its vectors are
// already live, so a raise arrives as an ordinary interrupt.
//
// g_contend PICKS WHICH STATE THE CHECK WANTS THIS CORE IN, and the two contending states are
// what make each half of the coupling checkable: interrupts open, so the vector is what answers,
// or the kernel lock taken under this core's own mask, where only the poll in the acquire loop
// can. The spinning cell is what tells the initiator this core is in the second.
//
// A SHARED KERNEL LEAVES THIS LOOP FOR GOOD once it has published a thread for this core.
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
    // the sender's, before this core waits for a doorbell it may already have missed.
    //
    // AHEAD OF THE UNMASK: the service body's only exclusion is this core's mask and the
    // vector reaches the same body, so a doorbell landing inside this call would re-enter
    // take_call or release_call. An SGI raised while it runs stays pending, so waiting
    // misses nothing.
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
        // AFTER THE MASK AND CLEARED BEFORE ITS RESTORE, so the flag is never set with this
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
