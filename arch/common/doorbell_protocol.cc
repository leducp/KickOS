// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The cross-core doorbell protocol shared by every backend: the request and answer cells, the
// raise, the rendezvous wait, and the primary's bring-up check of the kernel lock's coupling to
// the doorbell.
//
// The controller carries only the wake and the answer travels in the cells: no controller
// reached from here tells a sender that a target serviced a raise. Every cell has exactly one
// writer: a request word is written by the asking core, an answer word by the answering core.
//
// The service bodies stay in the backends: tests/static/check_route_service_order.sh matches an
// unindented `void <name>(` in three named files, so a body moved here reads as absent.
// arch_ipi_fence keeps its barrier in its own body, which tests/static/check_ipi_fence.sh reads.

#include <kickos/arch/doorbell_protocol.h>

#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)

#include <kickos/arch/arch.h>
#include <kickos/arch/klock_owner.h>

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
        // How long a peer may leave a request unanswered before it is taken as lost. Sized far
        // over, not tuned: under emulation without icount the guest clock tracks host time, and
        // a host that deschedules a peer's vCPU makes it late without making it dead. Keep it a
        // duration, never an iteration count: a spinning waiter's rate is not the peer's.
        constexpr uint64_t PEER_WAIT_NS = 5ull * 1000ull * 1000ull * 1000ull;
        // Spins between two clock reads in a rendezvous wait. A power of two, so the test is a
        // mask. An answer that lands within this many spins never reads the clock at all.
        constexpr uint32_t CLOCK_CHECK_SPINS = 256u;
        static_assert((CLOCK_CHECK_SPINS & (CLOCK_CHECK_SPINS - 1u)) == 0u, "tested as a mask");

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
        // Rounds with every peer's interrupts open, so the raise reaches the far side through
        // the vector. These are the only rounds in the image that put a peer there.
        constexpr uint32_t VECTOR_ROUNDS = 32u;
        // Rounds with every peer inside the acquire loop under its own mask, where only the
        // loop's poll can answer. Zero at one kernel core: arch_kernel_lock is an empty macro
        // there, so no acquire loop exists.
#if KICKOS_KERNEL_CORES > 1
        constexpr uint32_t POLL_ROUNDS = 32u;
#else
        constexpr uint32_t POLL_ROUNDS = 0u;
#endif
        constexpr uint32_t CHECK_ROUNDS = VECTOR_ROUNDS + POLL_ROUNDS;
        // The round count reaches the console as exactly two hex digits, and a gate parses it.
        static_assert(CHECK_ROUNDS <= 0xFFu, "the round count is printed in two digits");
        static_assert(KICKOS_NUM_CORES <= 0xFu, "a core count is printed in one digit");

        constexpr char CHECK_HEAD[] = "# doorbell: ";
        constexpr char CHECK_TAIL[] = " core(s) answered, rounds 0x";
        constexpr char CHECK_UNMOVED[] = ", unmoved 0x";

        // The sequence each peer owes this core, chosen before the raise and never read back
        // from the request cell afterwards: a send that stored nothing satisfies that readback
        // at 0 == 0.
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

        // One round's raise and rendezvous under the caller's lock. The postcondition is read
        // before the release: a peer catching up after arch_ipi_wait returned would satisfy a
        // count taken later.
        bool doorbell_round(uint32_t me, uint32_t peers, uint32_t peer_count)
        {
            uint32_t owed[KICKOS_DOORBELL_CORES] = {};
            owe_next(me, peers, owed);

            arch_ipi_send(peers);
            arch_ipi_wait(peers);

            return settled_against(me, peers, owed) == peer_count;
        }

        // The round's control: arch_ipi_send with its request bump deleted, so every peer takes
        // the raise, finds nothing owed and answers nothing. Returns how many of `peers` the
        // postcondition refused, which must be all of them.
        //
        // Run with every peer's interrupts open and the previous round settled, so an answer
        // cell that moves under it moved for this raise.
        uint32_t unmoved_under_a_still_request(uint32_t me, uint32_t peers, uint32_t peer_count)
        {
            uint32_t owed[KICKOS_DOORBELL_CORES] = {};
            owe_next(me, peers, owed);

            kickos_doorbell_raise(peers);
            arch_ipi_wait(peers);

            return peer_count - settled_against(me, peers, owed);
        }

#if KICKOS_KERNEL_CORES > 1
        // The set of `peers` observed inside the acquire loop under their own mask, waited for
        // until it is all of them or the bound expires.
        //
        // The caller must hold the kernel lock: a peer that has published cannot leave the loop
        // before the release, so the peers accumulate instead of passing through.
        uint32_t await_peers_spinning(uint32_t peers)
        {
            uint64_t const deadline = arch_clock_now() + PEER_WAIT_NS;
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

        // How many of `peers` have completed a kernel-lock acquisition, waited for until all have
        // or the bound expires. The caller must not hold the lock.
        uint32_t await_peers_held(uint32_t peers, uint32_t peer_count)
        {
            uint64_t const deadline = arch_clock_now() + PEER_WAIT_NS;
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

#if KICKOS_KERNEL_CORES > 1 && defined(KICKOS_DEBUG) && KICKOS_DEBUG
namespace kickos::klock
{
    uint32_t g_owner[KICKOS_NUM_CORES] = {};
}
#endif

extern "C"
{

// The caller's own bit is serviced here, not raised: a core waiting with interrupts masked would
// wait on a handler it keeps out. Its request cell is still bumped, so the answer cells describe
// the whole mask.
//
// AN AMP NODE RAISES ITS OWN BIT INSTEAD: the service runs kickos_amp_node_service, which sends
// through here, and no stack figure can price that recursion. Its kernel is one core, so no
// rendezvous waits on the answer.
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
#if KICKOS_AMP_NODE
    kickos_doorbell_raise(cores);
#else
    if ((cores & (1u << me)) != 0)
    {
        kickos_doorbell_poll();
    }
    kickos_doorbell_raise(cores & ~(1u << me));
#endif
}

// Spins on the answer cells, skipping the caller's own bit, which the send answered
// synchronously. Services this core's doorbell while spinning, so two initiators can wait on
// each other.
//
// The cells and never the raise: on a part whose wake can be erased by a set racing a clear, a
// raise-watching wait would sit until its bound expired and then kill the machine over a race
// whose whole cost is meant to be latency.
void arch_ipi_wait(uint32_t cores)
{
    using namespace kickos::doorbell;

    uint32_t const me = arch_doorbell_core();
    uint32_t const peers = cores & ~(1u << me);

    // Zero until the first clock read arms it, so the bound runs from when an answer was first
    // missing.
    uint64_t deadline = 0;
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
            if ((spins & (CLOCK_CHECK_SPINS - 1u)) == 0u)
            {
                uint64_t const now = arch_clock_now();
                if (deadline == 0)
                {
                    deadline = now + PEER_WAIT_NS;
                }
                else if (now > deadline)
                {
                    part_write(PART_UNANSWERED, sizeof(PART_UNANSWERED) - 1);
                    hex1(to);
                    part_write(NL, sizeof(NL) - 1);
                    kfault_terminate();
                }
            }
            kickos_doorbell_poll();
            part_spin();
        }
    }
}

bool arch_ipi_answered(uint32_t cores)
{
    using namespace kickos::doorbell;

    uint32_t const me = arch_doorbell_core();
    uint32_t const peers = cores & ~(1u << me);

    for (uint32_t to = 0; to < KICKOS_DOORBELL_CORES; to++)
    {
        if ((peers & (1u << to)) != 0
            and g_answer[to].seq[me].load() != g_request[me].seq[to].load())
        {
            return false;
        }
    }
    return true;
}

#if KICKOS_NUM_CORES > 1
// The primary's bring-up check of the doorbell, run once with every secondary parked. Every round
// holds the kernel lock across a raise and a rendezvous, so the far side answers while the
// initiator holds what the far side contends for.
//
// No round may depend on winning a race: each phase drives the peers into its state and waits,
// bounded, to observe them there. An expired bound is a mechanism failure and stays fatal.
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

    // Phase one, every peer's interrupts open: the raise arrives through the vector while the
    // lock is held, so a far side that took the lock deadlocks here instead of passing.
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

    // The control for both phases. It must run here: phase one has settled every cell and the
    // peers are still spinning with interrupts open.
    arch_kernel_lock();
    uint32_t const unmoved = unmoved_under_a_still_request(me, peers, peer_count);
    arch_kernel_unlock();

#if KICKOS_KERNEL_CORES > 1
    // Phase two, every peer inside the acquire loop under its own mask, where only the loop's
    // poll can answer. The overlap is awaited with the lock held, never hoped for across rounds.
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

    // Fail here: the wait below would spend a second bound on a peer that never started an
    // acquisition. Restore the park first so no peer is left contending for the lock.
    if (spinning_rounds != POLL_ROUNDS)
    {
        g_contend = CONTEND_PARK;
        part_write(PART_NO_SPIN, sizeof(PART_NO_SPIN) - 1);
        hex1(spinning_seen);
        part_write(NL, sizeof(NL) - 1);
        kfault_terminate();
    }

    // Only with the lock free can a contending peer complete its acquisition.
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

    // Unless the control refused every peer, the rounds above certify a protocol both sides
    // satisfy by standing still.
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

#if KICKOS_KERNEL_CORES > 1 && defined(KICKOS_DEBUG) && KICKOS_DEBUG
int arch_kernel_lock_held(void)
{
    return static_cast<int>(kickos::klock::g_owner[arch_cpu_id()]);
}
#endif

}

#endif
