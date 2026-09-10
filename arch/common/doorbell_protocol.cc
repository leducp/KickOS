// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The cross-core doorbell's protocol, shared by every backend that runs it: the request and
// answer cells, the raise, the rendezvous wait over them, and the primary's bring-up check of
// the coupling between the kernel lock and the doorbell.
//
// THE RENDEZVOUS IS SHARED MEMORY. No interrupt controller reached from here reports to a
// sender that a target has SERVICED a raise, so the controller carries the wake and the answer
// travels in the cells below.
//
// EVERY CELL HAS EXACTLY ONE WRITER: a request word is written by the core asking, an answer
// word by the core answering.
//
// WHAT STAYS IN A BACKEND, and none of it by oversight. The service body is pinned by file AND
// by name (tests/static/check_route_service_order.sh), whose reader matches an unindented
// `void <name>(` in one of three named files, so a body moved here would read as ABSENT.
// arch_ipi_fence keeps its barrier inside its own body, tests/static/check_ipi_fence.sh reading
// that body for the mnemonic and for the operand that separates a full barrier from a half. The
// clear, the re-raise a poll owes, the lock's claim and the secondary park are each spelled in
// the part's own instructions.

#include <kickos/arch/doorbell_protocol.h>

#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)

#include <kickos/arch/arch.h>

#include <kickos/sys/atomic.h>

#include <stddef.h>
#include <stdint.h>

extern "C" void kfault_terminate(void) __attribute__((noreturn));

namespace kickos::doorbell
{
    KICKOS_AMP_SHARED("cells.request") PartRow g_request[KICKOS_DOORBELL_CORES] = {};
    KICKOS_AMP_SHARED("cells.answer") PartRow g_answer[KICKOS_DOORBELL_CORES] = {};

    namespace
    {
        // Bounds a wait that can no longer be answered, so a lost raise REPORTS rather than
        // hanging the machine. Far above the handful of iterations an answer takes.
        constexpr uint32_t WAIT_SPINS = 4000000u;

        constexpr char NL[] = "\n";
    }

    bool pending(void)
    {
        uint32_t const me = arch_doorbell_core();
        for (uint32_t from = 0; from < KICKOS_DOORBELL_CORES; from++)
        {
            if (g_request[from].seq[me].load() != g_answer[me].seq[from].load())
            {
                return true;
            }
        }
        return false;
    }

    void hex1(uint32_t v)
    {
        char c = static_cast<char>('0' + (v & 0xFu));
        if ((v & 0xFu) > 9u)
        {
            c = static_cast<char>('a' + (v & 0xFu) - 10u);
        }
        part_write(&c, 1);
    }

#if KICKOS_NUM_CORES > 1
    Seq g_contend = {};
    PartCell g_held[KICKOS_NUM_CORES] = {};
    PartCell g_spinning[KICKOS_NUM_CORES] = {};

    namespace
    {
        // Rounds the bring-up check runs with every peer holding its interrupts OPEN, so a
        // raise reaches the far side through the vector. Nothing else in the image puts a peer
        // there.
        constexpr uint32_t VECTOR_ROUNDS = 32u;
        // Rounds it runs with every peer observed inside the acquire loop under its own
        // interrupt mask, where the poll in that loop is the only thing that can answer. ZERO
        // WITHOUT A KERNEL LOCK: at one kernel core arch_kernel_lock is an empty macro, so
        // there is no acquire loop to witness and no core can be found spinning in one.
#if KICKOS_KERNEL_CORES > 1
        constexpr uint32_t POLL_ROUNDS = 32u;
#else
        constexpr uint32_t POLL_ROUNDS = 0u;
#endif
        constexpr uint32_t CHECK_ROUNDS = VECTOR_ROUNDS + POLL_ROUNDS;
        // The round count reaches the console as exactly two hex digits, and a gate parses it.
        static_assert(CHECK_ROUNDS <= 0xFFu, "the round count is printed in two digits");
        static_assert(KICKOS_NUM_CORES <= 0xFu, "a core count is printed in one digit");

        // Sized far over rather than tuned: under emulation without icount the guest clock
        // tracks HOST time, so a contended host spends this budget while the guest barely
        // executes. A duration and never an iteration count for that same reason.
        constexpr uint64_t BRINGUP_WAIT_NS = 5ull * 1000ull * 1000ull * 1000ull;

        constexpr char CHECK_HEAD[] = "# doorbell: ";
        constexpr char CHECK_TAIL[] = " core(s) answered, rounds 0x";
        constexpr char CHECK_UNMOVED[] = ", unmoved 0x";

        // The sequence each peer owes this core, CHOSEN before the raise rather than read back
        // out of the request cell afterwards. A postcondition read off that cell is satisfied
        // at 0 == 0 by a send that stored nothing, which both sides reach by standing still;
        // this is the number that has to move for a round to settle.
        void owe_next(uint32_t me, uint32_t peers, uint32_t* owed)
        {
            for (uint32_t to = 0; to < KICKOS_DOORBELL_CORES; to++)
            {
                owed[to] = 0;
                if ((peers & (1u << to)) != 0)
                {
                    owed[to] = g_request[me].seq[to].load() + 1u;
                }
            }
        }

        // How many of `peers` have answered this core up to the sequence owe_next chose.
        uint32_t settled_against(uint32_t me, uint32_t peers, uint32_t const* owed)
        {
            uint32_t settled = 0;
            for (uint32_t to = 0; to < KICKOS_DOORBELL_CORES; to++)
            {
                if ((peers & (1u << to)) != 0 and g_answer[to].seq[me].load() == owed[to])
                {
                    settled++;
                }
            }
            return settled;
        }

        // One round's raise and rendezvous, WITH THE LOCK ALREADY HELD, and the postcondition
        // read before the caller releases it: what arch_ipi_wait owes is that every peer
        // answered THIS round's request before it returned, and a peer catching up afterwards
        // would satisfy a count taken at the end.
        bool doorbell_round(uint32_t me, uint32_t peers, uint32_t peer_count)
        {
            uint32_t owed[KICKOS_DOORBELL_CORES] = {};
            owe_next(me, peers, owed);

            arch_ipi_send(peers);
            arch_ipi_wait(peers);

            return settled_against(me, peers, owed) == peer_count;
        }

        // THE ROUND'S OWN CONTROL, and it plants the defect the postcondition exists to catch:
        // the raise made with the request cell left where it was, which is arch_ipi_send with
        // its bump deleted. Every peer takes the raise, finds nothing owed, answers nothing,
        // and arch_ipi_wait returns at once because it reads the same unmoved cell back.
        // Returns how many of `peers` the postcondition still refused, which must be all of
        // them: a round that settled here would certify a doorbell that never asked.
        //
        // RUN WITH EVERY PEER'S INTERRUPTS OPEN and with the previous round settled, so an
        // answer cell that moves under it moved for this raise.
        uint32_t unmoved_under_a_still_request(uint32_t me, uint32_t peers, uint32_t peer_count)
        {
            uint32_t owed[KICKOS_DOORBELL_CORES] = {};
            owe_next(me, peers, owed);

            kickos_doorbell_raise(peers);
            arch_ipi_wait(peers);

            return peer_count - settled_against(me, peers, owed);
        }

#if KICKOS_KERNEL_CORES > 1
        // Which of `peers` are inside the acquire loop under their own interrupt mask, waited
        // for until every one of them is or the bound expires. Returns the set observed, which
        // is `peers` exactly when the wait succeeded.
        //
        // THE CALLER HOLDS THE KERNEL LOCK, and that is what turns this from a race into an
        // arrival: a peer that has published cannot leave the window until the lock is
        // released, so the peers accumulate in it instead of passing through.
        uint32_t await_peers_spinning(uint32_t peers)
        {
            uint64_t const deadline = arch_clock_now() + BRINGUP_WAIT_NS;
            uint32_t seen = 0;
            while (seen != peers)
            {
                seen = 0;
                for (uint32_t core = 0; core < KICKOS_NUM_CORES; core++)
                {
                    if ((peers & (1u << core)) != 0 and g_spinning[core].v.load() != 0u)
                    {
                        seen |= 1u << core;
                    }
                }
                if (seen != peers and arch_clock_now() > deadline)
                {
                    return seen;
                }
                part_spin();
            }
            return seen;
        }

        // How many of `peers` have completed a kernel-lock acquisition, waited for until every
        // one of them has or the bound expires.
        //
        // THE CALLER MUST NOT HOLD THE LOCK: an acquisition is what this waits for, and the
        // holder is what would prevent it.
        uint32_t await_peers_held(uint32_t peers, uint32_t peer_count)
        {
            uint64_t const deadline = arch_clock_now() + BRINGUP_WAIT_NS;
            uint32_t held = 0;
            while (held != peer_count)
            {
                held = 0;
                for (uint32_t core = 0; core < KICKOS_NUM_CORES; core++)
                {
                    if ((peers & (1u << core)) != 0 and g_held[core].v.load() != 0u)
                    {
                        held++;
                    }
                }
                if (held != peer_count and arch_clock_now() > deadline)
                {
                    return held;
                }
                part_spin();
            }
            return held;
        }
#endif
    }
#endif
}

extern "C"
{

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
    using namespace kickos::doorbell;

    uint32_t const me = arch_doorbell_core();

    for (uint32_t to = 0; to < KICKOS_DOORBELL_CORES; to++)
    {
        if ((cores & (1u << to)) != 0)
        {
            // Single writer, so a load and a store rather than an increment.
            g_request[me].seq[to] = g_request[me].seq[to].load() + 1u;
        }
    }
    if ((cores & (1u << me)) != 0)
    {
        kickos_doorbell_poll();
    }
    kickos_doorbell_raise(cores & ~(1u << me));
}

// Spins on the answer cells alone, the calling core's own bit excepted: the send answered that
// one synchronously. SERVICES ITS OWN DOORBELL WHILE IT SPINS: two cores can each be an
// initiator waiting on the other.
//
// THE CELLS AND NEVER THE RAISE. On a part whose wake can be erased by a set racing a clear, a
// raise-watching wait would sit until its bound expired and then kill the machine over a race
// whose whole cost is meant to be latency.
void arch_ipi_wait(uint32_t cores)
{
    using namespace kickos::doorbell;

    uint32_t const me = arch_doorbell_core();
    uint32_t const peers = cores & ~(1u << me);

    for (uint32_t to = 0; to < KICKOS_DOORBELL_CORES; to++)
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
            if (spins > WAIT_SPINS)
            {
                part_write(PART_UNANSWERED, sizeof(PART_UNANSWERED) - 1);
                hex1(to);
                part_write(NL, sizeof(NL) - 1);
                kfault_terminate();
            }
            kickos_doorbell_poll();
            part_spin();
        }
    }
}

#if KICKOS_NUM_CORES > 1
// The primary's bring-up check of the mechanism the kernel is about to depend on, run once with
// every secondary parked. Every round holds the lock across a raise and a rendezvous, so the far
// side answers while the initiator holds what the far side is contending for.
//
// NO ROUND MAY DEPEND ON WINNING A RACE. Each phase drives the peers into the state it needs and
// WAITS, bounded, for them to be observed there; an expired bound is a peer that never got
// there, which is a mechanism failure and stays fatal.
void kickos_doorbell_selfcheck(void)
{
    using namespace kickos::doorbell;

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

    // PHASE ONE, EVERY PEER'S INTERRUPTS OPEN: the raise reaches the far side through the
    // vector, and the initiator holds the lock across it, so a far side that took the lock
    // would deadlock here rather than pass.
    g_contend = CONTEND_OPEN;
    for (uint32_t round = 0; round < VECTOR_ROUNDS; round++)
    {
        arch_kernel_lock();
        if (doorbell_round(me, peers, peer_count))
        {
            settled_rounds++;
        }
        arch_kernel_unlock();
    }

    // THE CONTROL FOR EVERY ROUND ABOVE AND BELOW, run here because phase one has just settled
    // every cell and phase two has not yet taken the peers out of their open-interrupt spin.
    arch_kernel_lock();
    uint32_t const unmoved = unmoved_under_a_still_request(me, peers, peer_count);
    arch_kernel_unlock();

#if KICKOS_KERNEL_CORES > 1
    // PHASE TWO, EVERY PEER INSIDE THE ACQUIRE LOOP UNDER ITS OWN MASK: the overlap is WAITED
    // FOR with the lock held rather than hoped for inside a round count, and the poll in that
    // loop is then the only thing that can answer.
    uint32_t spinning_rounds = 0;
    uint32_t spinning_seen = 0;
    g_contend = CONTEND_LOCK;
    for (uint32_t round = 0; round < POLL_ROUNDS; round++)
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
    // out that a peer never arrived, and the wait below would spend a second one on a peer that
    // cannot possibly complete an acquisition it never started. The park is restored first so a
    // peer is not left contending for a lock nothing will contend back.
    if (spinning_rounds != POLL_ROUNDS)
    {
        g_contend = CONTEND_PARK;
        part_write(PART_NO_SPIN, sizeof(PART_NO_SPIN) - 1);
        hex1(spinning_seen);
        part_write(NL, sizeof(NL) - 1);
        kfault_terminate();
    }

    // THE LOCK IS FREE FROM HERE, which is what lets a contending peer complete an
    // acquisition: the phase above held the word for every round it ran.
    uint32_t const contended = await_peers_held(peers, peer_count);
#endif
    g_contend = CONTEND_PARK;

    if (settled_rounds != CHECK_ROUNDS)
    {
        part_write(PART_EARLY_WAIT, sizeof(PART_EARLY_WAIT) - 1);
        hex1(settled_rounds / 16u);
        hex1(settled_rounds % 16u);
        part_write(NL, sizeof(NL) - 1);
        kfault_terminate();
    }

    // Every peer the control raised on refused to be counted, or the rounds above certify a
    // protocol both sides satisfy by standing still and the count they printed states nothing.
    if (unmoved != peer_count)
    {
        part_write(PART_STILL_SETTLED, sizeof(PART_STILL_SETTLED) - 1);
        hex1(peer_count - unmoved);
        part_write(NL, sizeof(NL) - 1);
        kfault_terminate();
    }

#if KICKOS_KERNEL_CORES > 1
    if (contended != peer_count)
    {
        part_write(PART_NO_CONTEND, sizeof(PART_NO_CONTEND) - 1);
        hex1(contended);
        part_write(NL, sizeof(NL) - 1);
        kfault_terminate();
    }
#endif

    part_write(CHECK_HEAD, sizeof(CHECK_HEAD) - 1);
    hex1(peer_count + 1u);
    part_write(CHECK_TAIL, sizeof(CHECK_TAIL) - 1);
    hex1(settled_rounds / 16u);
    hex1(settled_rounds % 16u);
    part_write(CHECK_UNMOVED, sizeof(CHECK_UNMOVED) - 1);
    hex1(unmoved);
    part_write(NL, sizeof(NL) - 1);
}
#endif

}

#endif
