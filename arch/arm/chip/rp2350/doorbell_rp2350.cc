// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The cross-core doorbell for the RP2350, and the core identity it is keyed on.
//
// A doorbell flag says a raise LANDED, never that the far side has serviced it, so SIO carries
// the wake and the answer travels in the cells below. Every cell has exactly one writer: the
// core asking writes a request word, the core answering writes an answer word.
//
// No row padding: this part caches only XIP and SRAM0-7 stripe on address bits 3:2 (RP2350
// datasheet RP-008373-DS-2, 2.2.3), so adjacent cells already answer from different banks.

#include <kickos/arch/arch.h>
#include <kickos/arch/doorbell_cells.h>

#include "regs/sio_mc.h"

#include <kickos/sys/atomic.h>

#include <stdint.h>

#if KICKOS_NUM_CORES > 1

namespace reg = kickos::rp2350::reg;

extern "C"
{
    void kfault_terminate(void) __attribute__((noreturn));
    // Defined below; node_vectors.S puts it in the doorbell line of node 1's table.
    void kickos_rp2350_doorbell_service(void);
}

namespace
{
    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    constexpr size_t RP2350_DOORBELL_LINE = alignof(uint32_t);

    using Seq = kickos::doorbell::Seq;
    using SeqRow = kickos::doorbell::Row<RP2350_DOORBELL_LINE>;

    // Both nodes write these, so under one image per node they sit in the region the two link
    // scripts agree on rather than being allocated per image.
    //
    // g_request[i].seq[t]: how many times core i has asked core t. Written by i, read by t.
    // g_answer[t].seq[i]: how far core t has answered core i. Written by t, read by i.
    KICKOS_AMP_SHARED("cells.request") SeqRow g_request[KICKOS_DOORBELL_CORES] = {};
    KICKOS_AMP_SHARED("cells.answer") SeqRow g_answer[KICKOS_DOORBELL_CORES] = {};

#if defined(KICKOS_ENABLE_SELFTEST)
    // Per-core doorbell services, read across nodes, so placed with the cells.
    KICKOS_AMP_SHARED("cells.served") SeqRow g_served[KICKOS_DOORBELL_CORES] = {};
#endif

    // One of the eight flags each way (datasheet 3.1.6). A core has exactly one peer here, so
    // OUT_SET carries no target field and one flag is the whole rendezvous.
    constexpr uint32_t DOORBELL_BIT = 1u;

    // Bounds a wait that can no longer be answered, so a lost raise REPORTS rather than
    // hanging the machine. Far above the handful of iterations an answer takes.
    constexpr uint32_t DOORBELL_WAIT_SPINS = 4000000u;

    char const WAIT_STUCK[] = "KickOS: rp2350 doorbell unanswered by core ";
    char const WAIT_STUCK_NL[] = "\n";

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

    // Masked: the body is not re-entrant against itself, an answer write preempted between its
    // read and its store publishing a stale sequence an initiator waits on forever.
    void doorbell_poll(void)
    {
        if (not doorbell_pending())
        {
            return;
        }
        arch_irq_state_t const state = arch_irq_save();
        kickos_rp2350_doorbell_service();
        arch_irq_restore(state);
    }
}

extern "C"
{

uint32_t arch_cpu_id(void)
{
    // 0 on core 0, 1 on core 1 (datasheet 3.1.2), read through this core's own SIO bank.
    uint32_t const id = r32(reg::sio::CPUID);
    if (id >= KICKOS_NUM_CORES)
    {
        return KICKOS_NUM_CORES - 1u;
    }
    return id;
}

// The far side of the doorbell, on the calling core. Reached from the node vector table
// (node_vectors.S) and from a poll inside a spin, and MASKED either way.
void kickos_rp2350_doorbell_service(void)
{
    uint32_t const me = arch_doorbell_core();

    // A raise landing after this clear stays pending and is delivered again. The DSB puts the
    // acknowledge on SIO ahead of the request reads, so a raise concurrent with it cannot be
    // both cleared here and missed by the loop below.
    uint32_t const raised = r32(reg::sio::DOORBELL_IN_CLR);
    r32(reg::sio::DOORBELL_IN_CLR) = raised;
    __asm volatile("dsb" ::: "memory");

#if defined(KICKOS_ENABLE_SELFTEST)
    g_served[me].seq[0] = g_served[me].seq[0].load() + 1u;
#endif
    for (uint32_t from = 0; from < KICKOS_DOORBELL_CORES; from++)
    {
        uint32_t const asked = g_request[from].seq[me].load();
        if (asked != g_answer[me].seq[from].load())
        {
            g_answer[me].seq[from] = asked;
        }
    }
#if KICKOS_AMP_NODE
    // AFTER THE ANSWERS, and that order is the contract: an AMP payload drain may not delay
    // the rendezvous an initiator waits on through this same body.
    kickos_amp_node_service();
#endif
}

// The calling core's own bit is serviced HERE rather than raised: a core that raised the
// doorbell on itself and then waited with interrupts masked would wait on a handler it is
// keeping out. Its request cell is bumped like any other, so the answer cells describe the
// whole mask.
void arch_ipi_send(uint32_t cores)
{
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
        doorbell_poll();
    }
    if ((cores & ~(1u << me)) != 0)
    {
        // The request cells are Normal memory and SIO is Device, which are reorderable
        // against each other: without this the raise can reach the peer ahead of the cell it
        // is about.
        __asm volatile("dsb" ::: "memory");
        r32(reg::sio::DOORBELL_OUT_SET) = DOORBELL_BIT;
    }
}

// A FULL barrier, and neither half of an acquire/release pair: the pairing it serves is a store
// then a load on both sides, which is the one direction release and acquire leave free.
// Reached through a plain call that no callgraph gate follows, so its body is asserted out of
// the linked image (tests/static/check_ipi_fence.sh).
void arch_ipi_fence(void)
{
    __asm volatile("dmb" ::: "memory");
}

// Spins on the answer cells alone, the calling core's own bit excepted: the send answered that
// one synchronously. SERVICES ITS OWN DOORBELL WHILE IT SPINS: two cores can each be an
// initiator waiting on the other.
void arch_ipi_wait(uint32_t cores)
{
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

#if defined(KICKOS_ENABLE_SELFTEST)
// Services in the low half, rendezvous initiated in the high half (arch.h, arch_ipi_counts).
// The high half is structurally zero here: no translating backend is compiled on this part.
uint64_t arch_ipi_counts(uint32_t core)
{
    if (core >= KICKOS_DOORBELL_CORES)
    {
        return 0;
    }
    return static_cast<uint64_t>(g_served[core].seq[0].load());
}
#endif

}

#endif
