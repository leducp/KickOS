// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/arch/arch.h>
#include <kickos/chip_limits.h>

#if KICKOS_KERNEL_CORES > 1

#include <kickos/arch/apic.h>
#include <kickos/arch/doorbell_protocol.h>
#include <kickos/arch/klock_owner.h>
#include <kickos/arch/regs.h>

#include <stdint.h>

#if KICKOS_BENCH
extern "C" void kickos_bench_lock_draw_clh(void);
#endif

namespace
{
    // One request per core plus the initially granted sentinel; the interrupt mask keeps at
    // most one request outstanding per core.
    struct alignas(64) ClhRequest
    {
        uint32_t pending;
    };

    struct alignas(64) ClhNode
    {
        ClhRequest* mine;
        ClhRequest* predecessor;
    };

    static_assert(sizeof(ClhRequest) == 64 and sizeof(ClhNode) == 64,
                  "CLH requests and nodes must not share cache lines");

    constinit ClhRequest g_clh_request[KICKOS_KERNEL_CORES + 1] = {};
    constinit ClhNode g_clh_node[KICKOS_KERNEL_CORES] = {};
    constinit ClhRequest* g_clh_tail = &g_clh_request[KICKOS_KERNEL_CORES];

    struct Part
    {
        // The poll leaves the delivery in the local APIC, whose vector then finds nothing owed.
        static void clear(void)
        {
        }
        static void raise_self(void)
        {
            kickos::x86_64::apic_doorbell();
        }
        static bool inject_owed(void)
        {
            return false;
        }
        static void sleep(void)
        {
            arch_idle_wait();
        }
        static void open(void)
        {
            arch_irq_window();
        }
        static void before_start(void)
        {
        }
    };
}

// Drops this core's non-global translations before the answer.
kickos::doorbell::Fenced kickos::doorbell::service_fence(Observed)
{
    kickos::x86_64::write_cr3(kickos::x86_64::read_cr3());
    return Fenced();
}

extern "C"
{

void kickos_doorbell_poll(void)
{
    kickos::doorbell::poll<Part>();
}

void kickos_doorbell_raise(uint32_t cores)
{
    kickos::x86_64::apic_doorbell_send(cores);
}

void arch_ipi_raise(uint32_t cores)
{
    uint32_t const me = arch_cpu_id();
    if ((cores & (1u << me)) != 0)
    {
        kickos::x86_64::apic_doorbell();
    }
    kickos::x86_64::apic_doorbell_send(cores & ~(1u << me));
}

// One poke and one wait over `peers`, each of which reloads CR3 in its service before it answers.
void kickos_x86_64_translation_rendezvous(uint32_t peers)
{
    arch_ipi_send(peers);
    arch_ipi_wait(peers);
}

void arch_ipi_fence(void)
{
    __asm__ volatile("mfence" ::: "memory");
}

void arch_kernel_lock(void)
{
    uint32_t const me = arch_cpu_id();
    ClhNode& node = g_clh_node[me];
    if (node.mine == nullptr)
    {
        node.mine = &g_clh_request[me];
    }
    __atomic_store_n(&node.mine->pending, 1u, __ATOMIC_RELAXED);
    node.predecessor = __atomic_exchange_n(&g_clh_tail, node.mine, __ATOMIC_ACQ_REL);
#if KICKOS_BENCH
    kickos_bench_lock_draw_clh();
#endif
    kickos::doorbell::await_turn(
        [&node] { return __atomic_load_n(&node.predecessor->pending, __ATOMIC_ACQUIRE) == 0u; });
    kickos::klock::owner_take();
}

void arch_kernel_unlock(void)
{
    kickos::klock::owner_drop();
    ClhNode& node = g_clh_node[arch_cpu_id()];
    __atomic_store_n(&node.mine->pending, 0u, __ATOMIC_RELEASE);
    node.mine = node.predecessor;
}

void kickos_x86_64_doorbell_park(void)
{
    kickos::doorbell::park<Part>();
}

}

#endif
