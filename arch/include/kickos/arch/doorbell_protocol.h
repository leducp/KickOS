// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The doorbell protocol a shared-kernel backend takes whole rather than spelling for itself:
// the cells, the raise over them, the rendezvous wait, the service body, the poll, the lock's
// turn wait, the secondary park, and the boot check of the coupling between the kernel lock and
// the doorbell.
//
// A BACKEND SUPPLIES TWO THINGS. What is the part's, through <kickos/arch/doorbell_part.h>: the
// padding a cell owes, the spin a wait costs and the tag each refusal names the part by. And the
// bodies and Part hooks below, which the protocol calls but cannot own.

#ifndef KICKOS_ARCH_DOORBELL_PROTOCOL_H
#define KICKOS_ARCH_DOORBELL_PROTOCOL_H

#include <kickos/arch/doorbell_cells.h>

#include <stdint.h>

#include <atomic>

// ONE guard over everything below, the part header included: this file ships from the
// always-installed root and <kickos/arch/doorbell_part.h> only for the configured arch, so an
// image with no doorbell has no part header and must still compile standalone
// (tests/integration/check_oot_export.sh). A second #if agreeing by hand would pass that gate.
#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)

#include <kickos/arch/doorbell_part.h>
#if KICKOS_KERNEL_CORES > 1
#include <kickos/arch/atomic_cas.h>
#endif

namespace kickos::doorbell
{
    using PartRow = Row<KICKOS_DOORBELL_LINE>;
    using PartCell = Cell<KICKOS_DOORBELL_LINE>;

    // How long a peer may leave a request unanswered before it is taken as lost. Sized far
    // over, not tuned: under emulation without icount the guest clock tracks host time, and
    // a host that deschedules a peer's vCPU makes it late without making it dead. Keep it a
    // duration, never an iteration count: a spinning waiter's rate is not the peer's.
    constexpr uint64_t PEER_WAIT_NS = 5ull * 1000ull * 1000ull * 1000ull;
    // A clock that has not moved across this many spins is dead, so a bounded wait ends anyway.
    constexpr uint32_t DEAD_CLOCK_SPINS = 1u << 24;

    // Both nodes write these, so under one image per node they sit in the region the two link
    // scripts agree on rather than being allocated per image.
    //
    // g_request[i].seq[t]: how many times core i has asked core t. Written by i, read by t.
    // g_answer[t].seq[i]: how far core t has answered core i. Written by t, read by i.
    extern PartRow g_request[KICKOS_DOORBELL_CORES];
    extern PartRow g_answer[KICKOS_DOORBELL_CORES];

    // Whether any peer has asked this core for something it has not answered.
    bool pending(void);

#if KICKOS_NUM_CORES > 1 && defined(KICKOS_WITNESS_DOORBELL) && KICKOS_WITNESS_DOORBELL
    // What the boot check asks of a parked secondary. 0 parks it in its sleep; 1 spins it,
    // opening its interrupts on every turn; 2 makes it take the lock under its own mask.
    //
    // 1 EXISTS TO GET THE PEERS OUT OF THAT SLEEP: a peer asleep there observes no store, so a
    // step straight to 2 would wait on cores that never read it. The first raise of the 1 phase
    // is what wakes them, and it is a phase whose witness is the vector taking that raise.
    constexpr uint32_t CONTEND_PARK = 0u;
    constexpr uint32_t CONTEND_OPEN = 1u;
    constexpr uint32_t CONTEND_LOCK = 2u;
    extern Seq g_contend;

    // g_held[i]: how many kernel-lock acquisitions core i has completed.
    //
    // g_spinning[i]: nonzero from before core i draws a ticket until it clears it under the lock
    // it then holds, its interrupts masked throughout. A READER HOLDING THE LOCK can conclude
    // the core is in the acquire loop and cannot leave it, which is what makes the overlap the
    // check needs an arrival to wait for rather than a race to win.
    extern PartCell g_held[KICKOS_NUM_CORES];
    extern PartCell g_spinning[KICKOS_NUM_CORES];
#endif

#if KICKOS_KERNEL_CORES > 1
    // The terminal panic's halt: g_halter is the halting core plus one, or 0; g_halted[i] that
    // core i has stopped; g_reachable[i] that core i services its doorbell, so the halter waits
    // on it.
    extern Seq g_halter;
    extern Seq g_halted[KICKOS_DOORBELL_CORES];
    extern Seq g_reachable[KICKOS_DOORBELL_CORES];

    // Masked caller. Never answers, never takes the kernel lock again.
    [[noreturn]] __attribute__((always_inline)) inline void halt_here(uint32_t me)
    {
        g_halted[me] = 1u;
        while (true)
        {
            part_spin();
        }
    }

    __attribute__((always_inline)) inline void halt_if_asked(uint32_t me)
    {
        uint32_t const halter = g_halter.load();
        if (halter != 0u and halter != me + 1u)
        {
            halt_here(me);
        }
    }

    // Before the core's first halt_if_asked: one of it and the halter sees the other's store.
    inline void reachable(uint32_t me)
    {
        g_reachable[me] = 1u;
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    // The claiming core plus one, or 0.
    extern uint32_t g_halt_claim;

    // kickos_doorbell_halt_peers's body. Request cells and not a bare raise: a peer spinning
    // masked for the kernel lock services only a request it finds pending. A second core halts
    // here, so two terminal panics never wait on each other. A peer not yet reachable is not
    // waited on: it halts on reaching park.
    inline void halt_peers(void)
    {
        uint32_t const me = arch_doorbell_core();
        // The claim carries its core: a panic nested on the claimant anywhere past the claim,
        // before its g_halter store included, must run the halt again, not halt the claimant.
        uint32_t claim = 0u;
        while (not arch_cas_u32(&g_halt_claim, &claim, me + 1u))
        {
            if (claim != 0u)
            {
                break;
            }
        }
        if (claim != 0u and claim != me + 1u)
        {
            halt_here(me);
        }
        g_halter = me + 1u;
        // Pairs with reachable(): a peer that missed g_halter is one this loop sees reachable.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        uint32_t const peers = KICKOS_CORE_SET_ALL & ~(1u << me);
        arch_ipi_send(peers);
        uint64_t const start = arch_clock_now();
        uint32_t still = 0;
        for (uint32_t core = 0; core < KICKOS_KERNEL_CORES; core++)
        {
            if ((peers & (1u << core)) == 0 or g_reachable[core].load() == 0u)
            {
                continue;
            }
            while (g_halted[core].load() == 0u)
            {
                uint64_t const now = arch_clock_now();
                if (now - start > PEER_WAIT_NS)
                {
                    return;
                }
                if (now == start)
                {
                    still++;
                    if (still >= DEAD_CLOCK_SPINS)
                    {
                        return;
                    }
                }
                part_spin();
            }
        }
    }
#endif

    // The service body's steps, each taking the token the step before it returns: snapshot,
    // fence, drain, answer. Minted by kickos_doorbell_service's own steps and by service_fence
    // alone.
    class Service;

    // That a snapshot was taken, and nothing else: the fence takes it in no register, so the call
    // costs the service no register held across it.
    class [[nodiscard]] Observed
    {
        friend class Snapshot;
        Observed()
        {
        }
    };

    // That this core has stored the request sequences it will answer from and never re-read: a
    // request raised after the snapshot is not one the fence attests to.
    class [[nodiscard]] Snapshot
    {
    public:
        bool owed(void) const
        {
            return owed_;
        }
        Observed observed(void) const
        {
            return Observed();
        }

    private:
        friend class Service;
        explicit Snapshot(bool owed) : owed_(owed)
        {
        }
        bool owed_;
    };

    class [[nodiscard]] __attribute__((warn_unused)) Fenced
    {
        friend Fenced service_fence(Observed);
        Fenced()
        {
        }
    };

    // The barrier this part owes between observing a request and answering it, on the calling
    // core. Defined by every backend whose service body is kickos_doorbell_service.
    Fenced service_fence(Observed);
}

extern "C"
{

// The far side of the doorbell, on the calling core: snapshot, fence, route drain, answers.
// MASKED, and not re-entrant against itself.
void kickos_doorbell_service(void);

// The far side of the doorbell run from a spin rather than from the vector: drops this part's
// pending state, runs the backend's service body, and raises again whatever the same cause
// still owes. MASKED THROUGHOUT, the service body not being re-entrant against itself.
void kickos_doorbell_poll(void);

// The part's lowering of the raise, over every core named in `cores`. The CALLING core's bit
// reaches here only on an AMP node; everywhere else arch_ipi_send services it inline.
void kickos_doorbell_raise(uint32_t cores);

#if KICKOS_KERNEL_CORES > 1
// The terminal panic's stop of every other kernel core, masked and for good, returning once each
// has stopped or PEER_WAIT_NS has passed. Masked caller; one that finds another core's halt under
// way stops instead.
void kickos_doorbell_halt_peers(void);

// The primary's reachable mark, once every peer has arrived; a secondary's is its park's.
void kickos_doorbell_reachable(void);
#endif

#if KICKOS_NUM_CORES > 1 && defined(KICKOS_WITNESS_DOORBELL) && KICKOS_WITNESS_DOORBELL
// The primary's one-shot boot check of the mechanism the kernel is about to depend on, run once
// with every secondary parked. Fatal on failure.
void kickos_doorbell_boot_check(void);
#endif

}

namespace kickos::doorbell
{
    // kickos_doorbell_poll's body. Part::clear drops this core's pending state,
    // Part::raise_self raises it again, and Part::inject_owed says whether a device-line post
    // rode the raise the clear dropped.
    template <typename Part>
    __attribute__((always_inline)) inline void poll(void)
    {
        if (not pending())
        {
            return;
        }
        arch_irq_state_t const state = arch_irq_save();
        // Before the service: a raise landing after the clear stays pending and is delivered.
        Part::clear();
        kickos_doorbell_service();
        // The clear dropped every cause riding the raise and the service answers one of them,
        // so whatever the cells still say is owed is raised again or lost.
        bool owed = Part::inject_owed();
#if KICKOS_KERNEL_CORES > 1
        if (kickos_kernel_core_resched_owed() != 0)
        {
            owed = true;
        }
#endif
        if (owed)
        {
            Part::raise_self();
        }
        arch_irq_restore(state);
    }

#if KICKOS_KERNEL_CORES > 1
    // The kernel lock's wait for its turn. The poll is what keeps the coupling sound: a caller
    // acquires with interrupts masked, so a raise aimed at this core is pending and
    // undeliverable while an initiator holding the lock waits on it.
    //
    // The turn test precedes the poll: once the turn has come the previous holder has released,
    // so no lock-holding initiator waits on this core's answer. The draw must carry no poll, so a
    // core observed spinning answers only once it has drawn.
    template <typename Turn>
    __attribute__((always_inline)) inline void await_turn(Turn turned)
    {
        while (not turned())
        {
            kickos_doorbell_poll();
            part_spin();
        }
    }
#endif

#if KICKOS_NUM_CORES > 1
    // Where a core with no thread to run waits for a doorbell. Part::sleep is entered masked
    // and unmasks and sleeps with nothing landing between the two; Part::open takes a pending
    // raise through the vector; Part::before_start readies this core for its scheduler.
    //
    // Under KICKOS_WITNESS_DOORBELL, g_contend picks the state the boot check wants this core
    // in: asleep, interrupts open so the vector answers, or the kernel lock taken under this
    // core's own mask, where only the poll in the turn wait can.
    template <typename Part>
    [[noreturn]] __attribute__((always_inline)) inline void park(void)
    {
#if KICKOS_KERNEL_CORES > 1 || (defined(KICKOS_WITNESS_DOORBELL) && KICKOS_WITNESS_DOORBELL)
        uint32_t const me = arch_cpu_id();
#endif
#if KICKOS_KERNEL_CORES > 1
        reachable(me);
#endif
        while (true)
        {
            // Masked across every test below. A raise taken between a test and the sleep is a
            // wake this core never sleeps on, and the core-start raise is a single edge.
            (void)arch_irq_save();
#if KICKOS_KERNEL_CORES > 1
            halt_if_asked(me);
            if (kickos_kernel_core_startable() != 0)
            {
                // Masked and never restored: the scheduler's first resume stands on a frame
                // carrying its own interrupt state.
                Part::before_start();
                kickos_kernel_core_start();
            }
#endif
#if defined(KICKOS_WITNESS_DOORBELL) && KICKOS_WITNESS_DOORBELL
            uint32_t const contend = g_contend.load();
            if (contend == CONTEND_PARK)
            {
                // Must unmask and sleep with nothing landing between the two.
                Part::sleep();
                continue;
            }
            if (contend == CONTEND_LOCK)
            {
                // Cleared under the lock: cleared after the release, it would still say so about
                // a core that has already left.
                g_spinning[me].v = 1u;
                arch_kernel_lock();
                g_held[me].v = g_held[me].v.load() + 1u;
                g_spinning[me].v = 0u;
                arch_kernel_unlock();
            }
            Part::open();
            part_spin();
#else
            // Must unmask and sleep with nothing landing between the two.
            Part::sleep();
#endif
        }
    }
#endif
}

#endif

#endif
