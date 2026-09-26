// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/arch/arch.h>

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
    // One request per core plus the initially granted sentinel. On release a
    // core gives its request to its successor and reuses the predecessor's.
    // The interrupt mask permits at most one outstanding request per core.
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
#if defined(KICKOS_ENABLE_SELFTEST)
    alignas(64) uint32_t g_served[KICKOS_KERNEL_CORES] = {};
#endif
}

extern "C"
{

// The caller and the interrupt handler enter with local interrupts masked. A
// snapshot is answered only after the local non-global translations have been
// dropped and the IRQ routing request has been serviced.
void kickos_x86_64_doorbell_service(void)
{
    using namespace kickos::doorbell;
    uint32_t const me = arch_cpu_id();
    uint32_t asked[KICKOS_DOORBELL_CORES] = {};
    bool owed = false;
    for (uint32_t from = 0; from < KICKOS_DOORBELL_CORES; ++from)
    {
        asked[from] = g_request[from].seq[me].load();
        owed |= asked[from] != g_answer[me].seq[from].load();
    }
    if (not owed)
    {
        return;
    }
    kickos::x86_64::write_cr3(kickos::x86_64::read_cr3());
    kickos_irq_route_service();
#if defined(KICKOS_ENABLE_SELFTEST)
    ++g_served[me];
#endif
    for (uint32_t from = 0; from < KICKOS_DOORBELL_CORES; ++from)
    {
        if (asked[from] != g_answer[me].seq[from].load())
        {
            g_answer[me].seq[from] = asked[from];
        }
    }
}

void kickos_doorbell_poll(void)
{
    if (not kickos::doorbell::pending())
    {
        return;
    }
    arch_irq_state_t const state = arch_irq_save();
    kickos_x86_64_doorbell_service();
    if (kickos_kernel_core_resched_owed() != 0)
    {
        kickos::x86_64::apic_doorbell();
    }
    arch_irq_restore(state);
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
    while (__atomic_load_n(&node.predecessor->pending, __ATOMIC_ACQUIRE) != 0u)
    {
        kickos_doorbell_poll();
        __asm__ volatile("pause" ::: "memory");
    }
    kickos::klock::owner_take();
}

void arch_kernel_unlock(void)
{
    kickos::klock::owner_drop();
    ClhNode& node = g_clh_node[arch_cpu_id()];
    __atomic_store_n(&node.mine->pending, 0u, __ATOMIC_RELEASE);
    node.mine = node.predecessor;
}

#if defined(KICKOS_ENABLE_SELFTEST)
uint64_t arch_ipi_counts(uint32_t core)
{
    if (core >= KICKOS_KERNEL_CORES)
    {
        return 0;
    }
    return __atomic_load_n(&g_served[core], __ATOMIC_RELAXED);
}

uint32_t arch_ipi_deferred(uint32_t)
{
    return 0;
}

uint32_t arch_ipi_seat_set(uint32_t, uint32_t)
{
    return ARCH_IPI_SEAT_NONE;
}
#endif

// A peer without a scheduled idle thread has only its local APIC doorbell live.
// It parks here until the primary publishes the scheduler seat.
void kickos_x86_64_doorbell_park(void)
{
    using namespace kickos::doorbell;
    __asm__ volatile("sti" ::: "memory");
    uint32_t const me = arch_cpu_id();
    while (true)
    {
        if (kickos_kernel_core_seated() != 0 and kickos_kernel_core_ready() != 0)
        {
            (void)arch_irq_save();
            kickos_kernel_core_arrive();
            kickos_kernel_core_start();
        }
        uint32_t const contend = g_contend.load();
        if (contend == CONTEND_PARK)
        {
            __asm__ volatile("hlt" ::: "memory");
            continue;
        }
        if (contend == CONTEND_OPEN)
        {
            __asm__ volatile("pause" ::: "memory");
            continue;
        }
        arch_irq_state_t const state = arch_irq_save();
        g_spinning[me].v = 1u;
        arch_kernel_lock();
        g_held[me].v = g_held[me].v.load() + 1u;
        arch_kernel_unlock();
        g_spinning[me].v = 0u;
        arch_irq_restore(state);
    }
}

}

#endif
