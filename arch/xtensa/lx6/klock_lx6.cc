// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The cross-core kernel lock and the doorbell it is coupled to, for the Xtensa LX6: the acquire
// loop services a pending doorbell, and the doorbell's far side must not take the lock.
//
// THE RENDEZVOUS IS SHARED MEMORY. The trigger register reports nothing back to a sender and is
// not even per target core, so the answer travels in the cells below.
//
// EVERY CELL HAS EXACTLY ONE WRITER: a request word is written by the core asking, an answer
// word by the core answering, and the lock word only through S32C1I inside this file. The
// trigger register is not such a cell and need not be: two cores only ever SET it.
//
// Nothing clears the trigger in hardware, so a sender setting it while a receiver clears it can
// erase the wake. That costs a SPURIOUS ENTRY and never a lost request, since the set then
// lands after the clear and the LEVEL input re-asserts. The service body must stay idempotent.
//
// NO CACHE MEANS NO LINE TO SHARE, so the rows below carry no padding. Do not add any: the two
// caches serve the external path alone and kernel state is in internal SRAM.

#include <kickos/arch/arch.h>
#include <kickos/arch/lx6_doorbell.h>

#if KICKOS_NUM_CORES > 1

#include <kickos/sys/atomic.h>

#include <stdint.h>

extern "C" void kfault_terminate(void) __attribute__((noreturn));

namespace
{
    using Seq = kickos::Atomic<uint32_t, kickos::Order::ACQUIRE | kickos::Order::RELEASE>;

    // One row per core, each row written by that core alone.
    struct SeqRow
    {
        Seq seq[KICKOS_NUM_CORES];
    };

    // g_request[i].seq[t]: how many times core i has asked core t. Written by i, read by t.
    // g_answer[t].seq[i]: how far core t has answered core i. Written by t, read by i.
    SeqRow g_request[KICKOS_NUM_CORES] = {};
    SeqRow g_answer[KICKOS_NUM_CORES] = {};

    // Bounds a wait that can no longer be answered, so a lost raise REPORTS rather than hanging
    // the machine. Far above the handful of iterations an answer takes.
    constexpr uint32_t DOORBELL_WAIT_SPINS = 4000000u;

    // Bounds the bring-up check's waits for a peer to reach the state a phase needs. Sized far
    // over: what is being waited on is another core executing a handful of instructions.
    constexpr uint64_t LX6_BRINGUP_WAIT_NS = 5ull * 1000ull * 1000ull * 1000ull;

    // Rounds the bring-up check runs with every peer holding its interrupts OPEN, so a raise
    // reaches the far side through the level-1 dispatch. Nothing else in the image puts a peer
    // there.
    constexpr uint32_t DOORBELL_VECTOR_ROUNDS = 32u;
    // Rounds it runs with every peer observed inside the acquire loop under its own interrupt
    // mask, where the poll in that loop is the only thing that can answer. ZERO WITHOUT A
    // KERNEL LOCK, where arch_kernel_lock is an empty macro and there is no loop to witness.
#if KICKOS_KERNEL_CORES > 1
    constexpr uint32_t DOORBELL_POLL_ROUNDS = 32u;
#else
    constexpr uint32_t DOORBELL_POLL_ROUNDS = 0u;
#endif
    constexpr uint32_t DOORBELL_CHECK_ROUNDS = DOORBELL_VECTOR_ROUNDS + DOORBELL_POLL_ROUNDS;
    // The round count reaches the console as exactly two hex digits, and the gate parses it.
    static_assert(DOORBELL_CHECK_ROUNDS <= 0xFFu, "the round count is printed in two digits");
    static_assert(KICKOS_NUM_CORES <= 0xFu, "a core count is printed in one digit");

    // What the check asks of a parked secondary. 0 parks it in WAITI; 1 spins it with its
    // interrupts open; 2 makes it take the lock under its own interrupt mask.
    //
    // 1 exists to get the peers out of WAITI, where they observe no store: a step straight to
    // 2 would wait on cores that never read it.
    constexpr uint32_t CONTEND_PARK = 0u;
    constexpr uint32_t CONTEND_OPEN = 1u;
    constexpr uint32_t CONTEND_LOCK = 2u;
    Seq g_contend = {};

    // One cell per core: the three below are written by their own core and read by any. The
    // matrix shape belongs to g_request and g_answer, whose second index is a real peer.

    // Per-core count of lock acquisitions.
    Seq g_held[KICKOS_NUM_CORES] = {};

    // g_spinning[i]: nonzero while core i is between publishing intent and releasing the
    // kernel lock, its interrupts masked throughout. A reader HOLDING THE LOCK can conclude the
    // core is in the acquire loop and cannot leave it.
    Seq g_spinning[KICKOS_NUM_CORES] = {};

#if defined(KICKOS_ENABLE_SELFTEST)
    // Per-core doorbell services. No counterpart for rendezvous initiated: see arch_ipi_counts.
    Seq g_served[KICKOS_NUM_CORES] = {};
#endif

    // SYNC, not the buffered writer: this fires from a spin that may be inside an interrupt
    // or under this core's own mask, where the thread-only buffered path does not belong.
    char const WAIT_STUCK[] = "KickOS: lx6 doorbell unanswered by core ";
    char const WAIT_STUCK_NL[] = "\n";
    char const CHECK_HEAD[] = "# doorbell: ";
    char const CHECK_TAIL[] = " core(s) answered, rounds 0x";
    char const CHECK_NL[] = "\n";
    char const EARLY_WAIT[] = "KickOS: lx6 doorbell wait returned unanswered, rounds 0x";
    char const NO_CONTEND[] = "KickOS: lx6 kernel lock uncontended, peers ";
    char const NO_SPIN[] = "KickOS: lx6 no peer reached the acquire loop, spinning mask 0x";

    void hex1(uint32_t v)
    {
        char c = static_cast<char>('0' + (v & 0xFu));
        if ((v & 0xFu) > 9u)
        {
            c = static_cast<char>('a' + (v & 0xFu) - 10u);
        }
        arch_console_write_sync(&c, 1);
    }

    // Nothing to yield to on this core: Xtensa has no hint instruction a spin can take, and
    // WAITI would sleep a core that must keep polling.
    inline void spin_hint(void)
    {
        __asm volatile("nop" ::: "memory");
    }

    // The park's resting posture: the two contending arms need this core's interrupts OPEN
    // whatever level it entered the park at, which arch_irq_restore would not give.
    inline void irq_open(void)
    {
        uint32_t ps = 0;
        __asm volatile("rsil %0, 0" : "=a"(ps) : : "memory");
        (void)ps;
    }

    // Masked: the body is not re-entrant against itself, an answer write preempted between its
    // read and its store publishing a stale sequence an initiator waits on forever.
    //
    // Decides from the CELLS and never from the trigger, a lost wake being what the set/clear
    // race can still cost.
    void doorbell_poll(void)
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

    // One round's raise and rendezvous, WITH THE LOCK ALREADY HELD and the postcondition read
    // before the caller releases it: a peer catching up afterwards would satisfy a count taken
    // at the end.
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
    // THE CALLER HOLDS THE KERNEL LOCK: a peer that has published cannot leave the window
    // until it is released, so the peers accumulate in it.
    uint32_t await_peers_spinning(uint32_t peers)
    {
        uint64_t const deadline = arch_clock_now() + LX6_BRINGUP_WAIT_NS;
        uint32_t seen = 0;
        while (seen != peers)
        {
            seen = 0;
            for (uint32_t core = 0; core < KICKOS_NUM_CORES; core++)
            {
                if ((peers & (1u << core)) != 0 and g_spinning[core].load() != 0u)
                {
                    seen |= 1u << core;
                }
            }
            if (seen != peers and arch_clock_now() > deadline)
            {
                return seen;
            }
            spin_hint();
        }
        return seen;
    }

    // How many of `peers` have completed a kernel-lock acquisition, waited for until every one
    // of them has or the bound expires.
    //
    // THE CALLER MUST NOT HOLD THE LOCK: an acquisition is what this waits for.
    uint32_t await_peers_held(uint32_t peers, uint32_t peer_count)
    {
        uint64_t const deadline = arch_clock_now() + LX6_BRINGUP_WAIT_NS;
        uint32_t held = 0;
        while (held != peer_count)
        {
            held = 0;
            for (uint32_t core = 0; core < KICKOS_NUM_CORES; core++)
            {
                if ((peers & (1u << core)) != 0 and g_held[core].load() != 0u)
                {
                    held++;
                }
            }
            if (held != peer_count and arch_clock_now() > deadline)
            {
                return held;
            }
            spin_hint();
        }
        return held;
    }
#endif
}

extern "C"
{

int kickos_lx6_doorbell_pending(void)
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

// The far side of the doorbell, on the calling core. Reached from the level-1 dispatch and from
// a poll inside a spin, and MASKED either way.
//
// THE CALLER HAS ALREADY CLEARED THE TRIGGER; this reads the cells after that clear.
void kickos_lx6_doorbell_service(void)
{
    uint32_t const me = arch_cpu_id();

    // Observed once and answered from that observation: answering a request raised after this
    // loop would report work not done.
    uint32_t asked[KICKOS_NUM_CORES] = {};
    bool owed = false;
    for (uint32_t from = 0; from < KICKOS_NUM_CORES; from++)
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
    g_served[me] = g_served[me].load() + 1u;
#endif

    // No instruction-side barrier: this part has no translation to invalidate and no cache
    // over the memory both cores fetch from.
#if KICKOS_KERNEL_CORES > 1
    // AFTER THE SNAPSHOT ABOVE AND BEFORE THE ANSWER STORES BELOW; both halves are the
    // contract (kernel/irq/irq_route.cc, line_op_ask).
    kickos_irq_route_service();
#endif

    for (uint32_t from = 0; from < KICKOS_NUM_CORES; from++)
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

// The calling core's own bit is serviced HERE: a core that raised the doorbell on itself and
// then waited with interrupts masked would wait on a handler it is keeping out. Its request
// cell is bumped like any other, so the answer cells describe the whole mask.
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
    kickos_lx6_doorbell_send(cores & ~(1u << me));
}

#if KICKOS_KERNEL_CORES > 1
// A core raises its own trigger: the matrix routes it to this core's own doorbell input, and
// the input is level, so a raise made under this core's mask stands until it unmasks.
void arch_ipi_resched_self(void)
{
    kickos_lx6_doorbell_send(1u << arch_cpu_id());
}
#endif

// Spins on the answer cells alone, the calling core's own bit excepted: the send answered that
// one synchronously. SERVICES ITS OWN DOORBELL WHILE IT SPINS: two cores can each be an
// initiator waiting on the other.
//
// THE CELLS AND NEVER THE TRIGGER. A wake the set-versus-clear race erased would leave a
// trigger-watching wait stuck until its bound expired and then kill the machine over a race
// whose whole cost is meant to be latency.
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
                arch_console_write_sync(WAIT_STUCK, sizeof(WAIT_STUCK) - 1);
                hex1(to);
                arch_console_write_sync(WAIT_STUCK_NL, sizeof(WAIT_STUCK_NL) - 1);
                kfault_terminate();
            }
            doorbell_poll();
            spin_hint();
        }
    }
}

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
    if (core >= KICKOS_NUM_CORES)
    {
        return 0;
    }
    return static_cast<uint64_t>(g_served[core].load());
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
        spin_hint();
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
            spin_hint();
            continue;
        }
        // SET UNDER THE MASK ABOVE AND CLEARED BEFORE THE REOPEN, so the flag is never set
        // with this core's interrupts open: that is the whole content of what a reader
        // concludes from it.
        g_spinning[me] = 1u;
        arch_kernel_lock();
        g_held[me] = g_held[me].load() + 1u;
        // CLEARED UNDER THE LOCK, ahead of the release. The flag's whole content is that this
        // core is between publishing intent and releasing the lock, and clearing it after the
        // release leaves a window in which it says so about a core that has already left.
        // A reader concludes from it only while IT holds the lock, so a clear published from
        // under the lock is one such a reader cannot catch half-done.
        g_spinning[me] = 0u;
        arch_kernel_unlock();
        irq_open();
        spin_hint();
    }
}

// The primary's bring-up check of the mechanism the kernel is about to depend on, run once with
// every secondary parked. Every round holds the lock across a raise and a rendezvous, so the far
// side answers while the initiator holds what the far side is contending for.
//
// NO ROUND MAY DEPEND ON WINNING A RACE. Each phase drives the peers into the state it needs and
// WAITS, bounded, for them to be observed there; an expired bound is a peer that never got
// there, which is a mechanism failure and stays fatal.
void kickos_lx6_doorbell_selfcheck(void)
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
    // level-1 dispatch, and the initiator holds the lock across it, so a far side that took the
    // lock would deadlock here rather than pass.
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

    // REPORTED WHERE IT IS KNOWN. await_peers_spinning has already spent its own bound finding
    // out that a peer never arrived, and the wait below spends a second one on a peer that
    // cannot possibly complete an acquisition it never started: ten seconds to print a verdict
    // reached at five. The park is restored first so a peer is not left contending for a lock
    // nothing will contend back.
    if (spinning_rounds != DOORBELL_POLL_ROUNDS)
    {
        g_contend = CONTEND_PARK;
        arch_console_write_sync(NO_SPIN, sizeof(NO_SPIN) - 1);
        hex1(spinning_seen);
        arch_console_write_sync(CHECK_NL, sizeof(CHECK_NL) - 1);
        kfault_terminate();
    }

    // THE LOCK IS FREE FROM HERE, which is what lets a contending peer complete an acquisition:
    // the phase above held the word for every round it ran.
    uint32_t const contended = await_peers_held(peers, peer_count);
#endif
    g_contend = CONTEND_PARK;

    if (settled_rounds != DOORBELL_CHECK_ROUNDS)
    {
        arch_console_write_sync(EARLY_WAIT, sizeof(EARLY_WAIT) - 1);
        hex1(settled_rounds / 16u);
        hex1(settled_rounds % 16u);
        arch_console_write_sync(CHECK_NL, sizeof(CHECK_NL) - 1);
        kfault_terminate();
    }

#if KICKOS_KERNEL_CORES > 1
    if (contended != peer_count)
    {
        arch_console_write_sync(NO_CONTEND, sizeof(NO_CONTEND) - 1);
        hex1(contended);
        arch_console_write_sync(CHECK_NL, sizeof(CHECK_NL) - 1);
        kfault_terminate();
    }
#endif

    arch_console_write_sync(CHECK_HEAD, sizeof(CHECK_HEAD) - 1);
    hex1(peer_count + 1u);
    arch_console_write_sync(CHECK_TAIL, sizeof(CHECK_TAIL) - 1);
    hex1(settled_rounds / 16u);
    hex1(settled_rounds % 16u);
    arch_console_write_sync(CHECK_NL, sizeof(CHECK_NL) - 1);
}

}

#endif
