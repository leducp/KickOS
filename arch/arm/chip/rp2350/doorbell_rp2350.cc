// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The RP2350's doorbell under the shared protocol, the core identity it is keyed on, and the AMP
// partition's launch of its peer. A SIO doorbell flag carries only the wake.

#include <kickos/arch/arch.h>
#include <kickos/chip_limits.h>
#include <kickos/arch/doorbell_protocol.h>
#include <kickos/io/mmio.h>

#include "accessctrl_rows.h"
#include "regs/accessctrl.h"
#include "regs/sio_mc.h"

#include <kickos/sys/partition_gate.h>

#include <stdint.h>

// Built whenever something rings the doorbell: a shared kernel's peers above one core, an AMP
// node's peer nodes at one core, where an own-image node drives a single core.
#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)

namespace reg = kickos::rp2350::reg;

namespace kickos
{
    void kprintf(char const* fmt, ...);
}

extern "C"
{
    void kfault_terminate(void) __attribute__((noreturn));
    void kpanic_enter(void);
}

namespace
{
    // One of the eight flags each way (datasheet 3.1.6). A core has exactly one peer here, so
    // OUT_SET carries no target field and one flag is the whole rendezvous.
    constexpr uint32_t DOORBELL_BIT = 1u;

    struct Part
    {
        // The DSB puts the acknowledge on SIO ahead of the request reads, so a raise concurrent
        // with it cannot be both cleared here and missed by the service.
        static void clear(void)
        {
            uint32_t const raised = r32(reg::sio::DOORBELL_IN_CLR);
            r32(reg::sio::DOORBELL_IN_CLR) = raised;
            __asm volatile("dsb" ::: "memory");
        }
        static void raise_self(void)
        {
            r32(reg::sio::DOORBELL_IN_SET) = DOORBELL_BIT;
        }
        static bool inject_owed(void)
        {
            return false;
        }
    };

#if KICKOS_AMP_OWN_IMAGE
    // FIFO_ST (datasheet Table 37, 3.1.11): bit 0 VLD, this core's RX FIFO holds a word;
    // bit 1 RDY, its TX FIFO has room.
    constexpr uint32_t FIFO_ST_VLD = 1u << 0;
    constexpr uint32_t FIFO_ST_RDY = 1u << 1;

    // Bounded: a partition whose second image was never flashed leaves core 1 in the bootrom's
    // wait, echoing nothing.
    constexpr uint32_t LAUNCH_SPINS = 4000000u;
    constexpr uint32_t LAUNCH_RESTARTS = 16u;

    char const LAUNCH_NO_ANSWER[] =
        "KickOS: RP2350 AMP node %u did not answer its launch handshake\n";
    char const LAUNCH_NO_SYNC[] =
        "KickOS: RP2350 AMP node %u never synchronised its launch handshake\n";

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

    [[noreturn]] void launch_failed(uint32_t node, char const* fmt)
    {
        kpanic_enter();
        kickos::kprintf(fmt, static_cast<unsigned>(node));
        kfault_terminate();
    }
#endif

#if KICKOS_AMP_OWN_IMAGE && (KICKOS_AMP_NODE_ID == 0)
    namespace gate = reg::accessctrl;
    namespace accessctrl = kickos::rp2350::accessctrl;

    constexpr uint32_t NODE_CORE[] = KICKOS_AMP_NODE_CORE_LIST;
    constexpr uint32_t NODE_CORE_COUNT = sizeof(NODE_CORE) / sizeof(NODE_CORE[0]);

    // ACCESSCTRL is reset by the bootrom on every boot path (Table 911, CFGRESET), so the image
    // is written on every boot. A peripheral no row names keeps its reset image, and LOCK is
    // never written: a lock bit holds until reset (Table 912).
    void gate_program(void)
    {
        uint16_t at = 0;
        accessctrl::Refusal const refusal =
            accessctrl::refusal_of(kickos_gate_rows, kickos_gate_row_count, NODE_CORE, NODE_CORE_COUNT, at);
        char const* why = nullptr;
        switch (refusal)
        {
            case accessctrl::Refusal::NONE:
            {
                break;
            }
            case accessctrl::Refusal::NODE:
            {
                why = "is granted to a node that runs on no core";
                break;
            }
            case accessctrl::Refusal::REGISTER:
            {
                why = "is no bus access register";
                break;
            }
            case accessctrl::Refusal::NEVER:
            {
                why = "is one no node is assigned";
                break;
            }
            case accessctrl::Refusal::TWICE:
            {
                why = "is named by two rows";
                break;
            }
        }
        if (why != nullptr)
        {
            kickos::kprintf("KickOS: RP2350 ACCESSCTRL 0x%x %s\n", static_cast<unsigned>(kickos_gate_rows[at].reg),
                            why);
            kfault_terminate();
        }
        for (uint32_t i = 0; i < kickos_gate_row_count; i++)
        {
            kickos_gate_row const& row = kickos_gate_rows[i];
            uintptr_t const reg_at = gate::BASE + row.reg;
            r32(reg_at) = accessctrl::image_of(r32(reg_at), NODE_CORE[row.node]);
            kickos::kprintf("# accessctrl: 0x%x = 0x%x\n", static_cast<unsigned>(row.reg),
                            static_cast<unsigned>(r32(reg_at)));
        }
    }
#endif
}

// No barrier: this part has no translation to invalidate and caches only XIP.
kickos::doorbell::Fenced kickos::doorbell::service_fence(Observed)
{
    return Fenced();
}

extern "C"
{

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

// The vector's entry to the shared service, in handler mode on SP_main. ITS DEPTH IS UNMEASURED:
// tests/static/trap_redzone_roots.txt roots no armv7m class on the MSP, so the descent through
// kickos_amp_node_service is bounded only by the _kernel_stack_size rp2350.ld reserves.
void kickos_rp2350_doorbell_service(void)
{
    Part::clear();
    kickos_doorbell_service();
}

void kickos_doorbell_poll(void)
{
    kickos::doorbell::poll<Part>();
}

// The calling core's own bit reaches here only on an AMP node, where its own IN flag carries the
// service once it unmasks.
void kickos_doorbell_raise(uint32_t cores)
{
    uint32_t const self = 1u << arch_doorbell_core();
    if ((cores & self) != 0)
    {
        r32(reg::sio::DOORBELL_IN_SET) = DOORBELL_BIT;
    }
    if ((cores & ~self) != 0)
    {
        // The request cells are Normal memory and SIO is Device, which are reorderable against
        // each other: without this the raise can reach the peer ahead of the cell it is about.
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

#if KICKOS_AMP_NODE_ID == 0
    gate_program();
#endif

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
                launch_failed(node, LAUNCH_NO_ANSWER);
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
                launch_failed(node, LAUNCH_NO_SYNC);
            }
        }
    }
}
#endif

}

#endif
