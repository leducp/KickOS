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

// Built whenever something rings the doorbell: a shared kernel's peers above one core, an AMP
// node's peer nodes at one core, where an own-image node drives a single core.
#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)

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

#if KICKOS_AMP_OWN_IMAGE
    // FIFO_ST (datasheet Table 37, 3.1.11): bit 0 VLD, this core's RX FIFO holds a word;
    // bit 1 RDY, its TX FIFO has room.
    constexpr uint32_t FIFO_ST_VLD = 1u << 0;
    constexpr uint32_t FIFO_ST_RDY = 1u << 1;

    // Bounded: a partition whose second image was never flashed leaves core 1 in the bootrom's
    // wait, echoing nothing.
    constexpr uint32_t LAUNCH_SPINS = 4000000u;
    constexpr uint32_t LAUNCH_RESTARTS = 16u;

    char const LAUNCH_HEAD[] = "KickOS: RP2350 AMP node ";
    char const LAUNCH_NO_ANSWER[] = " did not answer its launch handshake\r\n";
    char const LAUNCH_NO_SYNC[] = " never synchronised its launch handshake\r\n";

    // Polled and not WFE-parked: parking rests on the receiving core taking an event when its
    // RX FIFO fills, and a missed event there is this core asleep forever at boot.
    bool fifo_push(uint32_t v)
    {
        for (uint32_t spin = 0; spin < LAUNCH_SPINS; spin++)
        {
            if ((r32(reg::sio::FIFO_ST) & FIFO_ST_RDY) != 0u)
            {
                r32(reg::sio::FIFO_WR) = v;
                // SEV after every push: the peer's wait loop parks in WFE between reads, so a
                // push carrying no event leaves it asleep. The datasheet sequence (5.3) hides
                // this inside multicore_fifo_push_blocking(), so only the extra SEV before a
                // zero is visible there.
                __asm volatile("sev" ::: "memory");
                return true;
            }
        }
        return false;
    }

    bool fifo_pop(uint32_t* out)
    {
        for (uint32_t spin = 0; spin < LAUNCH_SPINS; spin++)
        {
            if ((r32(reg::sio::FIFO_ST) & FIFO_ST_VLD) != 0u)
            {
                *out = r32(reg::sio::FIFO_RD);
                return true;
            }
        }
        return false;
    }

    [[noreturn]] void launch_failed(uint32_t node, char const* tail, size_t tail_len)
    {
        arch_console_write(LAUNCH_HEAD, sizeof(LAUNCH_HEAD) - 1);
        char const digit = static_cast<char>('0' + (node % 10u));
        arch_console_write(&digit, 1);
        arch_console_write(tail, tail_len);
        kfault_terminate();
    }
#endif
}

extern "C"
{

// check_cpu_id_fold.sh scans source text with no preprocessor and treats this definition as
// guarded only inside an `#if KICKOS_NUM_CORES > 1` whose line ends there, so this guard may
// not be merged with the file guard above.
#if KICKOS_NUM_CORES > 1
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
#endif

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
#if KICKOS_AMP_OWN_IMAGE
        // Raised in hardware, never serviced inline: an inline call would put
        // kickos_amp_node_service under amp::send, which reaches here, and no red-zone figure
        // can price a recursion (tests/static/check_trap_redzone.sh). These are this core's own
        // IN bits, so its doorbell interrupt carries the service once it unmasks.
        r32(reg::sio::DOORBELL_IN_SET) = DOORBELL_BIT;
#else
        doorbell_poll();
#endif
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

// A real zero and not a stub's: a raise is one write to SIO DOORBELL_OUT_SET naming the other
// core of the pair, with no per-core state behind it that could hold one back.
uint32_t arch_ipi_deferred(uint32_t core)
{
    (void)core;
    return 0u;
}

// With no publication there is no seat to keep.
uint32_t arch_ipi_seat_set(uint32_t core, uint32_t seated)
{
    (void)core;
    (void)seated;
    return ARCH_IPI_SEAT_NONE;
}
#endif

#if KICKOS_AMP_OWN_IMAGE
// The partition primary launches its peer; the bootrom never does. Both cores enter the
// bootrom, and core 1 redirects early to a low-power wait for core 0 to hand it an entry point
// over the Secure SIO FIFO (datasheet 5.2, Table 451 "Core 1 Wait"). The started core is given
// its own vector table, stack pointer and entry, so it enters a second image in the same flash.
void arch_amp_release_peers(void)
{
    if (KICKOS_AMP_NODE_ID != 0)
    {
        return;
    }

    static_assert(KICKOS_AMP_PARTITION_CORES <= 2,
                  "the RP2350 has two cores and one SIO FIFO pair, so a partition spanning "
                  "more cores cannot launch its peers");

    // Every write this node made for a peer, ahead of the release that lets it read them.
    __asm volatile("dsb" ::: "memory");

    for (uint32_t node = 0; node < KICKOS_AMP_NODES; node++)
    {
        if (node == KICKOS_AMP_NODE_ID)
        {
            continue;
        }

        // The handshake's payload is the peer's vector table, whose first two words are its
        // initial SP and entry. rp2350.ld pins .isr_vector at the image base, and both images
        // live in one XIP window, so this node can read them directly.
        uintptr_t const vtor = static_cast<uintptr_t>(KICKOS_AMP_TEXT_BASE)
                               + static_cast<uintptr_t>(node)
                                     * static_cast<uintptr_t>(KICKOS_AMP_TEXT_SHARE);
        uint32_t const words[6] = {0u,
                                   0u,
                                   1u,
                                   static_cast<uint32_t>(vtor),
                                   r32(vtor + 0u),
                                   r32(vtor + 4u)};

        // The datasheet's sequence (5.3): six words, each echoed back before the next is sent,
        // and any mismatch restarts from the first. Core 1 may be anywhere in its own boot, so
        // the protocol is meant to be entered at any time and resynchronises by starting over.
        uint32_t seq = 0;
        uint32_t restarts = 0;
        while (seq < 6u)
        {
            uint32_t const cmd = words[seq];
            if (cmd == 0u)
            {
                // Drain this core's RX FIFO before a zero and signal an event: a peer waiting
                // for FIFO space never reads, so without both it echoes a stale word from a
                // previous attempt.
                while ((r32(reg::sio::FIFO_ST) & FIFO_ST_VLD) != 0u)
                {
                    // Assigned and not cast to void: a cast to void of a volatile lvalue does
                    // not access the object, so `(void)r32(...)` would never empty the FIFO and
                    // this loop would never exit.
                    uint32_t const drained = r32(reg::sio::FIFO_RD);
                    (void)drained;
                }
                __asm volatile("sev" ::: "memory");
            }

            uint32_t response = 0;
            if (not fifo_push(cmd) or not fifo_pop(&response))
            {
                launch_failed(node, LAUNCH_NO_ANSWER, sizeof(LAUNCH_NO_ANSWER) - 1);
            }
            if (response == cmd)
            {
                seq++;
                continue;
            }
            seq = 0;
            restarts++;
            if (restarts > LAUNCH_RESTARTS)
            {
                launch_failed(node, LAUNCH_NO_SYNC, sizeof(LAUNCH_NO_SYNC) - 1);
            }
        }
    }
}
#endif

}

#endif
