// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The cross-core kernel lock and the doorbell it is coupled to, for rv64imac: the acquire loop
// services a pending doorbell, and the doorbell's far side must not take the lock.
//
// THE RENDEZVOUS IS SHARED MEMORY. A CLINT msip word carries the wake and reports nothing back;
// the answer travels in the cells below, exactly as it does on the GIC backends.
//
// EVERY CELL HAS EXACTLY ONE WRITER: a request word is written by the core asking, an answer
// word by the core answering, and the lock word only through LR/SC inside this file.
//
// SSIP IS DOUBLE-BOOKED ON THIS ARCH and the dispatch is where that is resolved: the same cause
// carries a peer's raise and this hart's own device-line injection, so THE CELL and not the
// raise is what says a service is owed (kickos_rv64_doorbell_pending).

#include <kickos/arch/arch.h>
#include <kickos/arch/percpu.h>
#include <kickos/arch/rv64_doorbell.h>

#if KICKOS_NUM_CORES > 1

#include <kickos/sys/atomic.h>

#include <stddef.h>
#include <stdint.h>

extern "C" void kfault_terminate(void) __attribute__((noreturn));
extern "C" void kickos_rv64_init(void);

namespace
{
    // RISC-V publishes no cache-line width in any CSR, so this is a CHOICE sized to the
    // widest line the fleet's parts use rather than a fact read from the machine.
    constexpr size_t RV64_CACHE_LINE = 64u;

    using Seq = kickos::Atomic<uint32_t, kickos::Order::ACQUIRE | kickos::Order::RELEASE>;

    // One row per core, each row written by that core alone.
    struct alignas(RV64_CACHE_LINE) SeqRow
    {
        Seq seq[KICKOS_NUM_CORES];
    };
    static_assert(sizeof(SeqRow) % RV64_CACHE_LINE == 0,
                  "a row shorter than a line would share one with the next writer");

    // g_request[i].seq[t]: how many times core i has asked core t. Written by i, read by t.
    // g_answer[t].seq[i]: how far core t has answered core i. Written by t, read by i.
    SeqRow g_request[KICKOS_NUM_CORES] = {};
    SeqRow g_answer[KICKOS_NUM_CORES] = {};

    // Bounds a wait that can no longer be answered, so a lost raise REPORTS rather than hanging
    // the machine. Far above the handful of iterations an answer takes.
    constexpr uint32_t DOORBELL_WAIT_SPINS = 4000000u;

#if defined(KICKOS_ENABLE_SELFTEST)
    // Per-core doorbell services, and per-core instruction-side rendezvous initiated.
    SeqRow g_served[KICKOS_NUM_CORES] = {};
    SeqRow g_initiated[KICKOS_NUM_CORES] = {};
#endif

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
    // The round count reaches the console as exactly two hex digits, and a gate parses it.
    static_assert(DOORBELL_CHECK_ROUNDS <= 0xFFu, "the round count is printed in two digits");
    static_assert(KICKOS_NUM_CORES <= 0xFu, "a core count is printed in one digit");

    // Bring-up bound, sized far over rather than tuned: under emulation without icount the
    // guest clock tracks HOST time, so a contended host spends this budget while the guest
    // barely executes.
    constexpr uint64_t RV64_BRINGUP_WAIT_NS = 5ull * 1000ull * 1000ull * 1000ull;

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

#if KICKOS_KERNEL_CORES > 1
    // Per-core count of lock acquisitions.
    SeqRow g_held[KICKOS_NUM_CORES] = {};

    // g_spinning[i].seq[0]: nonzero while core i is between publishing intent and releasing the
    // kernel lock, its interrupts masked throughout. A READER HOLDING THE LOCK can conclude the
    // core is in the acquire loop and cannot leave it, which is what makes the overlap the check
    // needs an arrival to wait for rather than a race to win.
    SeqRow g_spinning[KICKOS_NUM_CORES] = {};
#endif

    // g_online[i].seq[0]: nonzero once hart i has reached the park. Written by that hart alone.
    SeqRow g_online[KICKOS_NUM_CORES] = {};

    // sie.STIE, which a parked secondary drops: it has armed no deadline and reaches no
    // scheduler, so a timer trap there would enter kickos_isr_timer with no core to serve.
    constexpr uint64_t SIE_STIE = 1ull << 5;

    char const WAIT_STUCK[] = "KickOS: rv64 doorbell unanswered by core ";
    char const WAIT_STUCK_NL[] = "\n";
    char const NO_SPIN[] = "KickOS: rv64 peers never reached the acquire loop, seen 0x";
    char const EARLY_WAIT[] = "KickOS: rv64 doorbell wait returned early, rounds settled 0x";
    char const NO_CONTEND[] = "KickOS: rv64 peers never completed an acquisition, held ";
    char const CHECK_HEAD[] = "# doorbell: ";
    char const CHECK_TAIL[] = " core(s) answered, rounds 0x";
    char const CHECK_NL[] = "\n";

    void hex1(uint32_t v)
    {
        char c = static_cast<char>('0' + (v & 0xFu));
        if ((v & 0xFu) > 9u)
        {
            c = static_cast<char>('a' + (v & 0xFu) - 10u);
        }
        arch_console_write(&c, 1);
    }

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

    // Runs the service body with this hart's interrupts masked: the body is not re-entrant
    // against itself, an answer write preempted between its read and its store publishing a
    // stale sequence that an initiator waits on forever.
    void doorbell_poll(void)
    {
        if (kickos_rv64_doorbell_pending() == 0)
        {
            return;
        }
        arch_irq_state_t const state = arch_irq_save();
        // BEFORE THE SERVICE: a raise landing after the clear stays pending and is delivered.
        doorbell_clear();
        kickos_rv64_doorbell_service();

        // THE CLEAR ABOVE DROPPED THE ONE CAUSE EVERY RAISE ARRIVES ON, and this body services
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

#if KICKOS_KERNEL_CORES > 1
    // The lock word: 0 free, 1 held, on a line of its own.
    alignas(RV64_CACHE_LINE) uint32_t g_kernel_lock = 0;

    // LR/SC over one coherent domain, which the A extension gives (Zalrsc in this board's
    // baseline). `res` is 0 only on a store-conditional that took the word, and the branch that
    // never stored leaves the 1 the first instruction put there.
    //
    // NOTHING MAY SIT BETWEEN THE LR AND THE SC that could make the reservation fail forever:
    // the sequence is a handful of instructions with no load, no branch backwards and no call,
    // which is the constrained form the ISA guarantees eventual success for.
    bool kernel_lock_claim(void)
    {
        uint32_t res = 1u;
        uint32_t cur = 0u;
        __asm volatile("       li      %0, 1\n"
                       "       lr.w.aq %1, (%2)\n"
                       "       bnez    %1, 1f\n"
                       "       sc.w    %0, %3, (%2)\n"
                       "1:\n"
                       : "=&r"(res), "=&r"(cur)
                       : "r"(&g_kernel_lock), "r"(1u)
                       : "memory");
        return res == 0u;
    }
#endif

#if defined(KICKOS_ENABLE_SELFTEST) && KICKOS_KERNEL_CORES > 1
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
    // until every one of them is or the bound expires.
    //
    // THE CALLER HOLDS THE KERNEL LOCK, and that is what turns this from a race into an
    // arrival: a peer that has published cannot leave the window until the lock is released.
    uint32_t await_peers_spinning(uint32_t peers)
    {
        uint64_t const deadline = arch_clock_now() + RV64_BRINGUP_WAIT_NS;
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
            __asm volatile("nop" ::: "memory");
        }
        return seen;
    }

    // How many of `peers` have completed a kernel-lock acquisition.
    //
    // THE CALLER MUST NOT HOLD THE LOCK: an acquisition is what this waits for, and the holder
    // is what would prevent it.
    uint32_t await_peers_held(uint32_t peers, uint32_t peer_count)
    {
        uint64_t const deadline = arch_clock_now() + RV64_BRINGUP_WAIT_NS;
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
            __asm volatile("nop" ::: "memory");
        }
        return held;
    }
#endif
#endif
}

extern "C"
{

int kickos_rv64_doorbell_pending(void)
{
    uint32_t const me = arch_cpu_id();
    for (uint32_t from = 0; from < KICKOS_NUM_CORES; from++)
    {
        if (g_request[from].seq[me].load() != g_answer[me].seq[from].load())
        {
            return 1;
        }
    }
    return 0;
}

// The far side of the doorbell, on the calling hart. Reached from the supervisor dispatch and
// from a poll inside a spin, and MASKED either way.
void kickos_rv64_doorbell_service(void)
{
    uint32_t const me = arch_cpu_id();
    // The translation half a peer owes for itself: SFENCE.VMA orders THIS hart's address
    // translation against another hart's table writes, and the ISA gives no operation by which
    // one hart performs it for another (Privileged ISA, "Supervisor Memory-Management Fence").
    // rs1 = rs2 = x0 because nothing here is tagged.
    //
    // Before the answer stores, so an initiator that has seen an answer has seen this.
    //
    // THE INSTRUCTION HALF IS NOT COVERED HERE: its operation is FENCE.I, and Zifencei is not
    // in this board's ISA baseline (arch/riscv/chip/virt_rv64/cpu.cmake).
    __asm volatile("sfence.vma zero, zero" ::: "memory");
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
    kickos_rv64_doorbell_send(cores & ~(1u << me));
}

#if KICKOS_KERNEL_CORES > 1
// sip.SSIP is supervisor-owned, so this hart raises its own doorbell directly rather than
// through the CLINT and the machine-mode trampoline a peer's raise needs.
void arch_ipi_resched_self(void)
{
    doorbell_raise_self();
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
            __asm volatile("nop" ::: "memory");
        }
    }
}

// One poke and one wait over `peers`, whose whole effect is the fence every serviced core runs.
void kickos_rv64_instruction_side_rendezvous(uint32_t peers)
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
        __asm volatile("nop" ::: "memory");
    }
}

// A release store, which the fence plus the plain store is on RISC-V: it pairs with the lr.w.aq
// the claim takes the word with, so everything done under the lock publishes before the word
// reads free.
void arch_kernel_unlock(void)
{
    __asm volatile("fence rw, w" ::: "memory");
    __asm volatile("sw zero, 0(%0)" ::"r"(&g_kernel_lock) : "memory");
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
        // AFTER THE MASK AND CLEARED BEFORE ITS RESTORE, so the flag is never set with this
        // hart's interrupts open: that is the whole content of what a reader concludes from it.
        g_spinning[me].seq[0] = 1u;
        arch_kernel_lock();
        g_held[me].seq[0] = g_held[me].seq[0].load() + 1u;
        arch_kernel_unlock();
        g_spinning[me].seq[0] = 0u;
        arch_irq_restore(state);
#endif
        __asm volatile("nop" ::: "memory");
    }
}

#if defined(KICKOS_ENABLE_SELFTEST) && KICKOS_KERNEL_CORES > 1
// The primary's bring-up check of the mechanism the kernel is about to depend on, run once with
// every secondary parked. Every round holds the lock across a raise and a rendezvous, so the far
// side answers while the initiator holds what the far side is contending for.
//
// NO ROUND MAY DEPEND ON WINNING A RACE. Each phase drives the peers into the state it needs and
// WAITS, bounded, for them to be observed there; an expired bound is a peer that never got
// there, which is a mechanism failure and stays fatal.
void kickos_rv64_doorbell_selfcheck(void)
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
#endif

// A released secondary's first supervisor code. kickos_rv64_init is entirely per-hart and is
// what seats this hart's identity, so it runs here exactly as it does on the boot hart.
uint32_t kickos_rv64_core_online_read(uint32_t id)
{
    if (id >= KICKOS_NUM_CORES)
    {
        return 0;
    }
    return g_online[id].seq[0].load();
}

void kickos_rv64_secondary_entry(void)
{
    arch_timer_disarm();
    kickos_rv64_init();

    uint32_t const id = arch_cpu_id();
    if (id != 0 and id < KICKOS_NUM_CORES)
    {
        g_online[id].seq[0] = 1u;
        kickos_rv64_doorbell_park();
    }
    while (true)
    {
        __asm volatile("wfi");
    }
}

}

#endif
