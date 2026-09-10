// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What the cross-core doorbell and the kernel lock coupled to it cost on the Xtensa LX6: the
// trigger's clear, the service body, the lock word's conditional store, and the secondary park.
// The protocol over them is in arch/common/doorbell_protocol.cc.
//
// THE RENDEZVOUS IS SHARED MEMORY. The trigger register reports nothing back to a sender and is
// not even per target core, so the answer travels in the shared cells. The trigger register is
// not such a cell and need not be: two cores only ever SET it. The lock word is written only
// through S32C1I inside this file.
//
// Nothing clears the trigger in hardware, so a sender setting it while a receiver clears it can
// erase the wake. That costs a SPURIOUS ENTRY and never a lost request, since the set then
// lands after the clear and the LEVEL input re-asserts. The service body must stay idempotent.

#include <kickos/arch/arch.h>
#include <kickos/arch/doorbell_protocol.h>
#include <kickos/arch/lx6_doorbell.h>

#if KICKOS_NUM_CORES > 1

#include <kickos/sys/atomic.h>

#include <stdint.h>

extern "C" void kfault_terminate(void) __attribute__((noreturn));

using kickos::doorbell::g_answer;
using kickos::doorbell::g_request;

namespace
{
#if defined(KICKOS_ENABLE_SELFTEST)
    // Per-core doorbell services. No counterpart for rendezvous initiated: see arch_ipi_counts.
    kickos::doorbell::PartCell g_served[KICKOS_DOORBELL_CORES] = {};
#endif

    // The park's resting posture: the two contending arms need this core's interrupts OPEN
    // whatever level it entered the park at, which arch_irq_restore would not give.
    inline void irq_open(void)
    {
        uint32_t ps = 0;
        __asm volatile("rsil %0, 0" : "=a"(ps) : : "memory");
        (void)ps;
    }

#if KICKOS_KERNEL_CORES > 1
    // The lock word: 0 free, 1 held. Kernel state, so the linker rule in esp32.ld is what puts
    // it where S32C1I's RCW transaction excludes the other CPU over it.
    uint32_t g_kernel_lock = 0;

    // S32C1I against SCOMPARE1 (Conditional Store Option). IT EXCLUDES ONLY WHILE ATOMCTL
    // SELECTS THE RCW BUS TRANSACTION, which kickos_lx6_init seats, refusing a core that cannot
    // confirm it.
    //
    // The instruction returns the value it READ, so a return of 0 is the one case where the
    // word was free and this core stored the 1. NO BARRIER EITHER SIDE: S32C1I plays the role
    // of both acquire and release on its own (ISA summary 4.3.13.5, p.122).
    bool kernel_lock_claim(void)
    {
        uint32_t ret = 1u;
        __asm volatile("wsr.scompare1 %[free]\n\t"
                       "s32c1i %[ret], %[addr], 0"
                       : [ret] "+a"(ret)
                       : [free] "a"(0u), [addr] "a"(&g_kernel_lock)
                       : "memory");
        return ret == 0u;
    }
#endif
}

extern "C"
{

int kickos_lx6_doorbell_pending(void)
{
    if (kickos::doorbell::pending())
    {
        return 1;
    }
    return 0;
}

// The far side of the doorbell, on the calling core. Reached from the level-1 dispatch and from
// a poll inside a spin, and MASKED either way.
//
// THE CALLER HAS ALREADY CLEARED THE TRIGGER; this reads the cells after that clear.
void kickos_lx6_doorbell_service(void)
{
    uint32_t const me = arch_doorbell_core();

    // Observed once and answered from that observation: answering a request raised after this
    // loop would report work not done.
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

#if defined(KICKOS_ENABLE_SELFTEST)
    g_served[me].v = g_served[me].v.load() + 1u;
#endif

    // No instruction-side barrier: this part has no translation to invalidate and no cache
    // over the memory both cores fetch from.
#if KICKOS_KERNEL_CORES > 1
    // AFTER THE SNAPSHOT ABOVE AND BEFORE THE ANSWER STORES BELOW; both halves are the
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
    // the rendezvous a shared kernel's callers wait on through this same body.
    kickos_amp_node_service();
#endif
}

// Masked: the body is not re-entrant against itself, an answer write preempted between its
// read and its store publishing a stale sequence an initiator waits on forever.
//
// Decides from the CELLS and never from the trigger, a lost wake being what the set/clear
// race can still cost.
void kickos_doorbell_poll(void)
{
    if (kickos_lx6_doorbell_pending() == 0)
    {
        return;
    }
    arch_irq_state_t const state = arch_irq_save();
    // BEFORE THE SERVICE: a set landing after this stays asserted and is delivered again.
    kickos_lx6_doorbell_clear();
    kickos_lx6_doorbell_service();
    arch_irq_restore(state);
}

void kickos_doorbell_raise(uint32_t cores)
{
    kickos_lx6_doorbell_send(cores);
}

#if KICKOS_KERNEL_CORES > 1
// A core raises its own trigger: the matrix routes it to this core's own doorbell input, and
// the input is level, so a raise made under this core's mask stands until it unmasks.
void arch_ipi_resched_self(void)
{
    kickos_lx6_doorbell_send(1u << arch_cpu_id());
}
#endif

#if defined(KICKOS_ENABLE_SELFTEST)
// Services in the low half, rendezvous initiated in the high half (arch.h, arch_ipi_counts).
//
// THE HIGH HALF IS STRUCTURALLY ZERO ON THIS BACKEND, and that is a fact about the part rather
// than a gap. A rendezvous exists to make a peer run maintenance it cannot be made to run any
// other way, and this part has neither half of it: no translation to invalidate, and no cache
// over the memory both cores fetch from. There is therefore no wrapper here pairing a send with
// a wait; a caller that ever needs one calls arch_ipi_send and arch_ipi_wait, which is what the
// seam splits them for.
uint64_t arch_ipi_counts(uint32_t core)
{
    if (core >= KICKOS_DOORBELL_CORES)
    {
        return 0;
    }
    return static_cast<uint64_t>(g_served[core].v.load());
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
        kickos::doorbell::part_spin();
    }
}

// S32RI, the ISA's store-release (Multiprocessor Synchronization Option, ISA summary 4.3.12,
// p.115), which the Conditional Store Option above has as a PREREQUISITE (4.3.13, p.118) and so
// is present wherever S32C1I is. Deliberately not the `memw` plus plain store the compiler
// emits for a release elsewhere in this image: one instruction, and it pairs visibly with the
// acquire half S32C1I already carries.
void arch_kernel_unlock(void)
{
    __asm volatile("s32ri %[zero], %[addr], 0"
                   :
                   : [zero] "a"(0u), [addr] "a"(&g_kernel_lock)
                   : "memory");
}
#endif

// Where a core with no thread to run waits for a doorbell. Its vectors, its mask and its matrix
// bank are already seated (kickos_lx6_init), so a raise arrives as an ordinary level-1
// interrupt.
//
// THE UNMASK IS HERE AND NOT IN THE CALLER: until kickos_lx6_init has pointed this core's bank
// at the doorbell, an arriving raise is a line this core has no handler for.
//
// NOTHING IS NARROWED ON THE WAY IN, and none is owed: INTENABLE is per core and kickos_lx6_init
// left this one carrying the doorbell alone. CCOMPARE0 is enabled by arch_timer_arm, which this
// core does not reach before its scheduler, and a device route is armed only where g_dev_core
// names this core, which the primary's binds do not.
//
// g_contend PICKS WHICH STATE THE CHECK WANTS THIS CORE IN, and the two contending states are
// what make each half of the coupling checkable: interrupts open, so the dispatch is what
// answers, or the kernel lock taken under this core's own mask, where only the poll in the
// acquire loop can. The spinning cell is what tells the initiator this core is in the second.
//
// A SHARED KERNEL LEAVES THIS LOOP FOR GOOD once it has published a thread for this core.
void kickos_lx6_doorbell_park(void)
{
    using namespace kickos::doorbell;

    uint32_t const me = arch_cpu_id();
    while (true)
    {
        // MASKED ACROSS EVERY TEST BELOW. A raise taken and cleared between a test and the
        // WAITI is a wake this core never sleeps on, and the core-start raise is a single
        // edge: the core would stay asleep until the primary's bound expired.
        (void)arch_irq_save();
#if KICKOS_KERNEL_CORES > 1
        if (kickos_kernel_core_seated() != 0 and kickos_kernel_core_ready() != 0)
        {
            // Masked and never restored: the scheduler's first resume stands on a frame
            // carrying its own interrupt state.
            kickos_kernel_core_arrive();
            kickos_kernel_core_start();
        }
#endif
        uint32_t const contend = g_contend.load();
        if (contend == CONTEND_PARK)
        {
            // WAITI TAKES PS.INTLEVEL FROM ITS IMMEDIATE, so this one instruction is the
            // unmask and the sleep together and nothing lands between them. A CORE IN WAITI
            // OBSERVES NO STORE, so the primary's publication alone cannot wake it: the
            // doorbell raise that follows the publication is what does, and this loop re-reads
            // the cells on the way back out, under the mask above.
            __asm volatile("waiti 0");
            continue;
        }
        if (contend == CONTEND_OPEN)
        {
            // No WAITI: this arm has to observe the next store, and it takes its raise through
            // the dispatch rather than through any poll of its own.
            irq_open();
            part_spin();
            continue;
        }
        // SET UNDER THE MASK ABOVE AND CLEARED BEFORE THE REOPEN, so the flag is never set
        // with this core's interrupts open: that is the whole content of what a reader
        // concludes from it.
        g_spinning[me].v = 1u;
        arch_kernel_lock();
        g_held[me].v = g_held[me].v.load() + 1u;
        // CLEARED UNDER THE LOCK, ahead of the release. The flag's whole content is that this
        // core is between publishing intent and releasing the lock, and clearing it after the
        // release leaves a window in which it says so about a core that has already left.
        // A reader concludes from it only while IT holds the lock, so a clear published from
        // under the lock is one such a reader cannot catch half-done.
        g_spinning[me].v = 0u;
        arch_kernel_unlock();
        irq_open();
        part_spin();
    }
}

}

#endif
