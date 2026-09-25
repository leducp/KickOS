// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What the cross-core doorbell and the kernel lock coupled to it cost on the Xtensa LX6: the
// trigger's clear, the service body, the ticket draw's conditional store, and the secondary
// park. The protocol over them is in arch/common/doorbell_protocol.cc.
//
// The rendezvous is shared memory. The trigger register reports nothing back to a sender and is
// not even per target core, so the answer travels in the shared cells. The trigger register is
// not such a cell and need not be: two cores only ever set it. The lock is a ticket pair: a
// draw takes g_next_ticket with S32C1I, and the holder alone advances g_now_serving with L32I
// and S32RI. Both counters are written only inside this file.
//
// Nothing clears the trigger in hardware, so a sender setting it while a receiver clears it can
// erase the wake. That costs a SPURIOUS ENTRY and never a lost request, since the set then
// lands after the clear and the LEVEL input re-asserts. The service body must stay idempotent.

#include <kickos/arch/arch.h>
#include <kickos/arch/doorbell_protocol.h>
#include <kickos/arch/klock_owner.h>
#include <kickos/arch/lx6_doorbell.h>

#if KICKOS_NUM_CORES > 1

#include <kickos/sys/atomic.h>

#include <stdint.h>

extern "C" void kfault_terminate(void) __attribute__((noreturn));
#if KICKOS_BENCH && KICKOS_KERNEL_CORES > 1
// Declared rather than included: this TU is below <kickos/bench.h>.
extern "C" void kickos_bench_lock_draw(uint32_t retries, uint32_t queued);
#endif

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
    // Adjacent and unpadded: KICKOS_DOORBELL_LINE is 4 on this part because no cache covers the
    // internal SRAM kernel state lives in, so there is no line for two counters to share and
    // padding would only cost bytes on the part with the least RAM. Kernel state, so esp32.ld's
    // internal-SRAM range assert is what keeps both where S32C1I's RCW transaction excludes the
    // other CPU.
    uint32_t g_next_ticket = 0;
    uint32_t g_now_serving = 0;

    // S32C1I against SCOMPARE1 (Conditional Store Option). It excludes only while ATOMCTL
    // selects the RCW bus transaction, which kickos_lx6_init seats, refusing a core that cannot
    // confirm it.
    //
    // The load stays inside the loop: S32C1I usually returns the current memory value, but the
    // ISA summary (p.120) allows a few implementations to return the bitwise NOT of SCOMPARE1
    // instead when the store is not done, so a hoisted load is not portable across them.
    //
    // The instruction plays the role of both acquire and release on its own (4.3.13.5, p.122),
    // but it sits on the DRAW here, which grants nothing: the acquire is owed by the wait.
    uint32_t kernel_lock_draw(uint32_t& retries)
    {
        uint32_t cur = 0u;
        uint32_t want = 0u;
#if KICKOS_BENCH
        uint32_t tries = 0u;
        __asm volatile("1:  addi %[n], %[n], 1\n\t"
                       "    l32ai %[cur], %[addr], 0\n\t"
                       "    wsr.scompare1 %[cur]\n\t"
                       "    addi %[want], %[cur], 1\n\t"
                       "    s32c1i %[want], %[addr], 0\n\t"
                       "    bne %[want], %[cur], 1b"
                       : [cur] "=&a"(cur), [want] "=&a"(want), [n] "+a"(tries)
                       : [addr] "a"(&g_next_ticket)
                       : "memory");
        retries = tries - 1u;
#else
        __asm volatile("1:  l32ai %[cur], %[addr], 0\n\t"
                       "    wsr.scompare1 %[cur]\n\t"
                       "    addi %[want], %[cur], 1\n\t"
                       "    s32c1i %[want], %[addr], 0\n\t"
                       "    bne %[want], %[cur], 1b"
                       : [cur] "=&a"(cur), [want] "=&a"(want)
                       : [addr] "a"(&g_next_ticket)
                       : "memory");
        (void)retries;
#endif
        return cur;
    }

    // L32AI, the Multiprocessor Synchronization Option's load-acquire (ISA summary 4.3.12,
    // Table 49, p.117), and the acquire half of the lock.
    uint32_t now_serving(void)
    {
        uint32_t v = 0u;
        __asm volatile("l32ai %[v], %[addr], 0"
                       : [v] "=a"(v)
                       : [addr] "a"(&g_now_serving)
                       : "memory");
        return v;
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
// a poll inside a spin, and masked either way.
//
// The caller has already cleared the trigger; this reads the cells after that clear.
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
    // rendezvous a shared kernel's callers wait on through this same body.
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
    // Before the service: a set landing after this stays asserted and is delivered again.
    kickos_lx6_doorbell_clear();
    kickos_lx6_doorbell_service();
#if KICKOS_KERNEL_CORES > 1
    // After the clear that absorbed it. A self raise sets this same trigger, so the clear above
    // drops a reschedule this core owes itself. klock_leave carries it for an ordinary critical
    // section; the park and the selfcheck release through arch_kernel_unlock and read no cell,
    // so without this the ask stays owed with the trigger clear.
    if (kickos_kernel_core_resched_owed() != 0)
    {
        arch_ipi_raise(1u << arch_cpu_id());
    }
    // A peer's device-line post rides the same trigger, and the clear dropped it too.
    if (kickos_lx6_inject_owed() != 0)
    {
        kickos_lx6_doorbell_send(1u << arch_cpu_id());
    }
#endif
    arch_irq_restore(state);
}

void kickos_doorbell_raise(uint32_t cores)
{
    kickos_lx6_doorbell_send(cores);
}

#if KICKOS_KERNEL_CORES > 1
// A core's own trigger is routed to its own doorbell input, and the input is level, so a raise
// made under this core's mask stands until it unmasks.
void arch_ipi_raise(uint32_t cores)
{
    kickos_lx6_doorbell_send(cores);
}
#endif

#if defined(KICKOS_ENABLE_SELFTEST)
// Services in the low half, rendezvous initiated in the high half (arch.h, arch_ipi_counts).
//
// The high half is structurally zero on this backend, and that is a fact about the part rather
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
// The poll in this loop is what keeps the coupling sound: a caller acquires with interrupts
// masked, so a raise aimed at this core is pending and undeliverable while an initiator holding
// the lock waits on it.
//
// The turn test precedes the poll: when the turn has come the previous holder has released, so
// no lock-holding initiator can be waiting on this core's answer and the skipped poll strands
// nobody. The draw carries no poll either, so a core observed spinning answers only once it has
// finished drawing.
void arch_kernel_lock(void)
{
    uint32_t retries = 0u;
    uint32_t const ticket = kernel_lock_draw(retries);
#if KICKOS_BENCH
    kickos_bench_lock_draw(retries, ticket - now_serving());
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

// S32RI, the ISA's store-release (Multiprocessor Synchronization Option, ISA summary 4.3.12,
// p.115), which the Conditional Store Option above has as a PREREQUISITE (4.3.13, p.118) and so
// is present wherever S32C1I is. Deliberately not the `memw` plus plain store the compiler
// emits for a release elsewhere in this image: it pairs visibly with the L32AI the wait reads
// the turn with.
void arch_kernel_unlock(void)
{
    kickos::klock::owner_drop();
    uint32_t v = 0u;
    __asm volatile("l32i  %[v], %[addr], 0\n\t"
                   "addi  %[v], %[v], 1\n\t"
                   "s32ri %[v], %[addr], 0"
                   : [v] "=&a"(v)
                   : [addr] "a"(&g_now_serving)
                   : "memory");
}
#endif

// Where a core with no thread to run waits for a doorbell. Its vectors, its mask and its matrix
// bank are already seated (kickos_lx6_init), so a raise arrives as an ordinary level-1
// interrupt.
//
// The unmask is here and not in the caller: until kickos_lx6_init has pointed this core's bank
// at the doorbell, an arriving raise is a line this core has no handler for.
//
// Nothing is narrowed on the way in, and none is owed: INTENABLE is per core and kickos_lx6_init
// left this one carrying the doorbell alone. CCOMPARE0 is enabled by arch_timer_arm, which this
// core does not reach before its scheduler, and a device route is armed only where g_dev_core
// names this core, which the primary's binds do not.
//
// g_contend picks which state the check wants this core in, and the two contending states are
// what make each half of the coupling checkable: interrupts open, so the dispatch is what
// answers, or the kernel lock taken under this core's own mask, where only the poll in the
// acquire loop can. The spinning cell is what tells the initiator this core is in the second.
//
// A shared kernel leaves this loop for good once it has published a thread for this core.
void kickos_lx6_doorbell_park(void)
{
    using namespace kickos::doorbell;

    uint32_t const me = arch_cpu_id();
    while (true)
    {
        // Masked across every test below. A raise taken and cleared between a test and the
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
            // WAITI takes PS.INTLEVEL from its immediate, so this one instruction is the
            // unmask and the sleep together and nothing lands between them. A core in WAITI
            // observes no store, so the primary's publication alone cannot wake it: the
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
        // Set under the mask above and cleared before the reopen, so the flag is never set
        // with this core's interrupts open: that is the whole content of what a reader
        // concludes from it.
        g_spinning[me].v = 1u;
        arch_kernel_lock();
        g_held[me].v = g_held[me].v.load() + 1u;
        // Cleared under the lock, ahead of the release. The flag's whole content is that this
        // core is between publishing intent and releasing the lock, and clearing it after the
        // release leaves a window in which it says so about a core that has already left.
        // A reader concludes from it only while it holds the lock, so a clear published from
        // under the lock is one such a reader cannot catch half-done.
        g_spinning[me].v = 0u;
        arch_kernel_unlock();
        irq_open();
        part_spin();
    }
}

}

#endif
