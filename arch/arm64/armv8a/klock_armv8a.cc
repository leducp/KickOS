// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The cross-core kernel lock and the doorbell it is coupled to, for armv8a: the acquire loop
// services a pending doorbell, and the doorbell's far side must not take the lock.
//
// THE RENDEZVOUS IS SHARED MEMORY. Neither GIC version reports that a target has SERVICED a
// software-generated interrupt, and GICv2's per-source pending registers are banked to the
// accessing core: the controller carries the wake, the answer travels in the cells below.
//
// EVERY CELL HAS EXACTLY ONE WRITER: a request word is written by the core asking, an answer
// word by the core answering, and the lock word only through LDAXR/STXR inside this file.

#include <kickos/arch/arch.h>

#include "../common/gic.h"
#include "../common/smp_bringup.h"

#include <kickos/sys/atomic.h>

#include <stddef.h>
#include <stdint.h>

#if KICKOS_NUM_CORES > 1

extern "C"
{
    void kfault_terminate(void) __attribute__((noreturn));
}

namespace
{
    // A53 cache line.
    constexpr size_t ARMV8A_CACHE_LINE = 64u;

    using Seq = kickos::Atomic<uint32_t, kickos::Order::ACQUIRE | kickos::Order::RELEASE>;

    // One row per core, each row written by that core alone.
    struct alignas(ARMV8A_CACHE_LINE) SeqRow
    {
        Seq seq[KICKOS_NUM_CORES];
    };
    static_assert(sizeof(SeqRow) % ARMV8A_CACHE_LINE == 0,
                  "a row shorter than a line would share one with the next writer");

    // g_request[i].seq[t]: how many times core i has asked core t. Written by i, read by t.
    // g_answer[t].seq[i]: how far core t has answered core i. Written by t, read by i.
    SeqRow g_request[KICKOS_NUM_CORES] = {};
    SeqRow g_answer[KICKOS_NUM_CORES] = {};

    // Bounds a wait that can no longer be answered, so a lost raise REPORTS rather than hanging
    // the machine. Far above the handful of iterations an answer takes.
    constexpr uint32_t DOORBELL_WAIT_SPINS = 4000000u;

    // Rounds the bring-up check runs with every peer holding its interrupts OPEN, so a raise
    // reaches the far side through the vector. Nothing else in the image puts a peer there.
    constexpr uint32_t DOORBELL_VECTOR_ROUNDS = 32u;
    // Rounds it runs with every peer observed inside the acquire loop under its own interrupt
    // mask, where the poll in that loop is the only thing that can answer. ZERO WITHOUT A
    // KERNEL LOCK: at one kernel core arch_kernel_lock is an empty macro, so there is no
    // acquire loop to witness and no core can be found spinning in one.
#if KICKOS_KERNEL_CORES > 1
    constexpr uint32_t DOORBELL_POLL_ROUNDS = 32u;
#else
    constexpr uint32_t DOORBELL_POLL_ROUNDS = 0u;
#endif
    constexpr uint32_t DOORBELL_CHECK_ROUNDS = DOORBELL_VECTOR_ROUNDS + DOORBELL_POLL_ROUNDS;
    // The round count reaches the console as exactly two hex digits, and the gate parses it.
    static_assert(DOORBELL_CHECK_ROUNDS <= 0xFFu, "the round count is printed in two digits");
    static_assert(KICKOS_NUM_CORES <= 0xFu, "a core count is printed in one digit");

    // What the check asks of a parked secondary. 0 parks it in WFI; 1 spins it with its
    // interrupts open; 2 makes it take the lock under its own interrupt mask.
    //
    // 1 EXISTS TO GET THE PEERS OUT OF WFI: a peer asleep there observes no store, so a step
    // straight to 2 would wait on cores that never read it. The first raise of the 1 phase is
    // what wakes them, and it is a phase whose witness is the vector taking that raise.
    constexpr uint32_t CONTEND_PARK = 0u;
    constexpr uint32_t CONTEND_OPEN = 1u;
    constexpr uint32_t CONTEND_LOCK = 2u;
    Seq g_contend = {};

    // Per-core count of lock acquisitions.
    SeqRow g_held[KICKOS_NUM_CORES] = {};

    // g_spinning[i].seq[0]: nonzero while core i is between publishing intent and releasing the
    // kernel lock, its interrupts masked throughout. A READER HOLDING THE LOCK can conclude the
    // core is in the acquire loop and cannot leave it, which is what makes the overlap the check
    // needs an arrival to wait for rather than a race to win.
    SeqRow g_spinning[KICKOS_NUM_CORES] = {};

#if defined(KICKOS_ENABLE_SELFTEST)
    // Per-core doorbell services, and per-core instruction-side rendezvous initiated.
    SeqRow g_served[KICKOS_NUM_CORES] = {};
    SeqRow g_initiated[KICKOS_NUM_CORES] = {};
#endif

    char const WAIT_STUCK[] = "KickOS: armv8a doorbell unanswered by core ";
    char const WAIT_STUCK_NL[] = "\n";
    char const CHECK_HEAD[] = "# doorbell: ";
    char const CHECK_TAIL[] = " core(s) answered, rounds 0x";
    char const CHECK_NL[] = "\n";
    char const EARLY_WAIT[] = "KickOS: armv8a doorbell wait returned unanswered, rounds 0x";
    char const NO_CONTEND[] = "KickOS: armv8a kernel lock uncontended, peers ";
    char const NO_SPIN[] = "KickOS: armv8a no peer reached the acquire loop, spinning mask 0x";

    void hex1(uint32_t v)
    {
        char c = static_cast<char>('0' + (v & 0xFu));
        if ((v & 0xFu) > 9u)
        {
            c = static_cast<char>('a' + (v & 0xFu) - 10u);
        }
        arch_console_write(&c, 1);
    }

    // Whether any peer has asked this core for something it has not answered.
    bool doorbell_pending(void)
    {
        uint32_t const me = arch_cpu_id();
        for (uint32_t from = 0; from < KICKOS_NUM_CORES; from++)
        {
            if (g_request[from].seq[me].load() != g_answer[me].seq[from].load())
            {
                return true;
            }
        }
        return false;
    }

    // Runs the service body with this core's interrupts masked: the body is not re-entrant
    // against itself, an answer write preempted between its read and its store publishing a
    // stale sequence that an initiator waits on forever.
    void doorbell_poll(void)
    {
        if (not doorbell_pending())
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

#if KICKOS_KERNEL_CORES > 1
    // The lock word: 0 free, 1 held, on a line of its own.
    alignas(ARMV8A_CACHE_LINE) uint32_t g_kernel_lock = 0;

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

    // One round's raise and rendezvous, WITH THE LOCK ALREADY HELD, and the postcondition read
    // before the caller releases it: what arch_ipi_wait owes is that every peer answered THIS
    // round's request before it returned, and a peer catching up afterwards would satisfy a
    // count taken at the end.
    bool doorbell_round(uint32_t me, uint32_t peers, uint32_t peer_count)
    {
        arch_ipi_send(peers);
        arch_ipi_wait(peers);

        uint32_t settled = 0;
        for (uint32_t to = 0; to < KICKOS_NUM_CORES; to++)
        {
            if ((peers & (1u << to)) != 0
                and g_answer[to].seq[me].load() == g_request[me].seq[to].load())
            {
                settled++;
            }
        }
        return settled == peer_count;
    }

#if KICKOS_KERNEL_CORES > 1
    // Which of `peers` are inside the acquire loop under their own interrupt mask, waited for
    // until every one of them is or the bound expires. Returns the set observed, which is
    // `peers` exactly when the wait succeeded.
    //
    // THE CALLER HOLDS THE KERNEL LOCK, and that is what turns this from a race into an
    // arrival: a peer that has published cannot leave the window until the lock is released, so
    // the peers accumulate in it instead of passing through.
    uint32_t await_peers_spinning(uint32_t peers)
    {
        uint64_t const deadline = arch_clock_now() + kickos::ARM64_BRINGUP_WAIT_NS;
        uint32_t seen = 0;
        while (seen != peers)
        {
            seen = 0;
            for (uint32_t core = 0; core < KICKOS_NUM_CORES; core++)
            {
                if ((peers & (1u << core)) != 0 and g_spinning[core].seq[0].load() != 0u)
                {
                    seen |= 1u << core;
                }
            }
            if (seen != peers and arch_clock_now() > deadline)
            {
                return seen;
            }
            __asm volatile("yield" ::: "memory");
        }
        return seen;
    }

    // How many of `peers` have completed a kernel-lock acquisition, waited for until every one
    // of them has or the bound expires.
    //
    // THE CALLER MUST NOT HOLD THE LOCK: an acquisition is what this waits for, and the holder
    // is what would prevent it.
    uint32_t await_peers_held(uint32_t peers, uint32_t peer_count)
    {
        uint64_t const deadline = arch_clock_now() + kickos::ARM64_BRINGUP_WAIT_NS;
        uint32_t held = 0;
        while (held != peer_count)
        {
            held = 0;
            for (uint32_t core = 0; core < KICKOS_NUM_CORES; core++)
            {
                if ((peers & (1u << core)) != 0 and g_held[core].seq[0].load() != 0u)
                {
                    held++;
                }
            }
            if (held != peer_count and arch_clock_now() > deadline)
            {
                return held;
            }
            __asm volatile("yield" ::: "memory");
        }
        return held;
    }
#endif
}

extern "C"
{

// The far side of the doorbell, on the calling core. Reached from the SGI handler and from a
// poll inside a spin, and MASKED either way.
void kickos_arm64_doorbell_service(void)
{
    uint32_t const me = arch_cpu_id();
    // A Context synchronization event on THIS PE: until a PE takes one, instructions it has
    // already fetched may be re-executed with no bound (DDI 0487 M.b section B2.7.4.2), and no
    // operation makes one PE synchronize another (Glossary, "Context Synchronization event").
    // ISB flushes the pipeline in the PE and IS such an event (section C6.2.177).
    //
    // EXPLICIT: doorbell_poll runs this body from inside a spin, which enters no exception, and
    // whether exception entry is itself such an event rests on FEAT_ExS and SCTLR_EL1.EIS.
    //
    // Before the answer stores, so an initiator that has seen an answer has seen this.
    __asm volatile("isb" ::: "memory");
#if defined(KICKOS_ENABLE_SELFTEST)
    g_served[me].seq[0] = g_served[me].seq[0].load() + 1u;
#endif
    for (uint32_t from = 0; from < KICKOS_NUM_CORES; from++)
    {
        uint32_t const asked = g_request[from].seq[me].load();
        if (asked != g_answer[me].seq[from].load())
        {
            g_answer[me].seq[from] = asked;
        }
    }
#if KICKOS_AMP_NODE
    // AFTER THE ANSWERS, and that order is the contract: an AMP payload drain may not delay
    // the rendezvous a shared kernel's callers wait on through this same body.
    kickos_amp_node_service();
#endif
}

// The calling core's own bit is serviced HERE rather than raised: a core that raised the
// doorbell on itself and then waited with interrupts masked would wait on a handler it is
// keeping out. Its request cell is bumped like any other, so the answer cells describe the
// whole mask.
//
// A caller wanting a target rescheduled publishes that ahead of this call
// (kickos::klock_resched_ask): the raise is an edge, and the acquire loop's poll absorbs it
// without entering any scheduler.
void arch_ipi_send(uint32_t cores)
{
    uint32_t const me = arch_cpu_id();

    for (uint32_t to = 0; to < KICKOS_NUM_CORES; to++)
    {
        if ((cores & (1u << to)) != 0)
        {
            // Single writer, so a load and a store rather than an increment.
            g_request[me].seq[to] = g_request[me].seq[to].load() + 1u;
        }
    }
    if ((cores & (1u << me)) != 0)
    {
        doorbell_poll();
    }
    kickos_armv8a_gic_doorbell_send(cores & ~(1u << me));
}

#if KICKOS_KERNEL_CORES > 1
// GICD_SGIR reaches the sending core like any other target, and the pending state is one bit,
// so a raise over one already pending is idempotent.
void arch_ipi_resched_self(void)
{
    kickos_armv8a_gic_doorbell_send(1u << arch_cpu_id());
}
#endif

// Spins on the answer cells alone, the calling core's own bit excepted: the send answered that
// one synchronously. SERVICES ITS OWN DOORBELL WHILE IT SPINS: two cores can each be an
// initiator waiting on the other.
void arch_ipi_wait(uint32_t cores)
{
    uint32_t const me = arch_cpu_id();
    uint32_t const peers = cores & ~(1u << me);

    for (uint32_t to = 0; to < KICKOS_NUM_CORES; to++)
    {
        if ((peers & (1u << to)) == 0)
        {
            continue;
        }
        uint32_t const asked = g_request[me].seq[to].load();
        uint32_t spins = 0;
        while (g_answer[to].seq[me].load() != asked)
        {
            spins++;
            if (spins > DOORBELL_WAIT_SPINS)
            {
                arch_console_write(WAIT_STUCK, sizeof(WAIT_STUCK) - 1);
                hex1(to);
                arch_console_write(WAIT_STUCK_NL, sizeof(WAIT_STUCK_NL) - 1);
                kfault_terminate();
            }
            doorbell_poll();
            __asm volatile("yield" ::: "memory");
        }
    }
}

// One poke and one wait over `peers`, whose whole effect is the ISB every serviced core runs.
void kickos_arm64_instruction_side_rendezvous(uint32_t peers)
{
#if defined(KICKOS_ENABLE_SELFTEST)
    if (peers != 0)
    {
        uint32_t const me = arch_cpu_id();
        g_initiated[me].seq[0] = g_initiated[me].seq[0].load() + 1u;
    }
#endif
    arch_ipi_send(peers);
    arch_ipi_wait(peers);
}

#if defined(KICKOS_ENABLE_SELFTEST)
// Services in the low half, rendezvous initiated in the high half (arch.h, arch_ipi_counts).
uint64_t arch_ipi_counts(uint32_t core)
{
    if (core >= KICKOS_NUM_CORES)
    {
        return 0;
    }
    return (static_cast<uint64_t>(g_initiated[core].seq[0].load()) << 32)
           | static_cast<uint64_t>(g_served[core].seq[0].load());
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
        doorbell_poll();
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
void kickos_armv8a_doorbell_park(void)
{
    // This core's own interface enabled the timer PPI, so masking its bank down to the doorbell
    // is what keeps kickos_isr_timer out of a core that reaches no scheduler.
    kickos_armv8a_gic_doorbell_only();
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
        g_spinning[me].seq[0] = 1u;
        arch_kernel_lock();
        g_held[me].seq[0] = g_held[me].seq[0].load() + 1u;
        arch_kernel_unlock();
        g_spinning[me].seq[0] = 0u;
        arch_irq_restore(state);
        __asm volatile("yield" ::: "memory");
    }
}

// The primary's bring-up check of the mechanism the kernel is about to depend on, run once with
// every secondary parked. Every round holds the lock across a raise and a rendezvous, so the far
// side answers while the initiator holds what the far side is contending for.
//
// NO ROUND MAY DEPEND ON WINNING A RACE. Each phase drives the peers into the state it needs and
// WAITS, bounded, for them to be observed there; an expired bound is a peer that never got
// there, which is a mechanism failure and stays fatal.
void kickos_armv8a_doorbell_selfcheck(void)
{
    uint32_t const me = arch_cpu_id();
    uint32_t peers = 0;
    uint32_t peer_count = 0;
    for (uint32_t core = 0; core < KICKOS_NUM_CORES; core++)
    {
        if (core != me)
        {
            peers |= 1u << core;
            peer_count++;
        }
    }

    uint32_t settled_rounds = 0;
    uint32_t spinning_rounds = 0;
    uint32_t spinning_seen = 0;

    // PHASE ONE, EVERY PEER'S INTERRUPTS OPEN: the raise reaches the far side through the
    // vector, and the initiator holds the lock across it, so a far side that took the lock
    // would deadlock here rather than pass.
    g_contend = CONTEND_OPEN;
    for (uint32_t round = 0; round < DOORBELL_VECTOR_ROUNDS; round++)
    {
        arch_kernel_lock();
        if (doorbell_round(me, peers, peer_count))
        {
            settled_rounds++;
        }
        arch_kernel_unlock();
    }

#if KICKOS_KERNEL_CORES > 1
    // PHASE TWO, EVERY PEER INSIDE THE ACQUIRE LOOP UNDER ITS OWN MASK: the overlap is WAITED
    // FOR with the lock held rather than hoped for inside a round count, and the poll in that
    // loop is then the only thing that can answer.
    g_contend = CONTEND_LOCK;
    for (uint32_t round = 0; round < DOORBELL_POLL_ROUNDS; round++)
    {
        arch_kernel_lock();
        spinning_seen = await_peers_spinning(peers);
        if (spinning_seen == peers)
        {
            spinning_rounds++;
            if (doorbell_round(me, peers, peer_count))
            {
                settled_rounds++;
            }
        }
        arch_kernel_unlock();
        if (spinning_seen != peers)
        {
            break;
        }
    }

    // THE LOCK IS FREE FROM HERE, which is what lets a contending peer complete an
    // acquisition: the phase above held the word for every round it ran.
    uint32_t const contended = await_peers_held(peers, peer_count);
#endif
    g_contend = CONTEND_PARK;

    if (spinning_rounds != DOORBELL_POLL_ROUNDS)
    {
        arch_console_write(NO_SPIN, sizeof(NO_SPIN) - 1);
        hex1(spinning_seen);
        arch_console_write(CHECK_NL, sizeof(CHECK_NL) - 1);
        kfault_terminate();
    }

    if (settled_rounds != DOORBELL_CHECK_ROUNDS)
    {
        arch_console_write(EARLY_WAIT, sizeof(EARLY_WAIT) - 1);
        hex1(settled_rounds / 16u);
        hex1(settled_rounds % 16u);
        arch_console_write(CHECK_NL, sizeof(CHECK_NL) - 1);
        kfault_terminate();
    }

#if KICKOS_KERNEL_CORES > 1
    if (contended != peer_count)
    {
        arch_console_write(NO_CONTEND, sizeof(NO_CONTEND) - 1);
        hex1(contended);
        arch_console_write(CHECK_NL, sizeof(CHECK_NL) - 1);
        kfault_terminate();
    }
#endif

    arch_console_write(CHECK_HEAD, sizeof(CHECK_HEAD) - 1);
    hex1(peer_count + 1u);
    arch_console_write(CHECK_TAIL, sizeof(CHECK_TAIL) - 1);
    hex1(settled_rounds / 16u);
    hex1(settled_rounds % 16u);
    arch_console_write(CHECK_NL, sizeof(CHECK_NL) - 1);
}

}

#endif
