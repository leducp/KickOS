// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The cross-core doorbell and the kernel lock coupled to it, on armv8a: the GIC's clear and
// raise, the service's context synchronization event, the ticket draw's exclusive pair, and the
// secondary park. The protocol over them is in arch/common/doorbell_protocol.cc.
//
// Neither GIC version reports that a target has serviced a software-generated interrupt, so
// the controller carries the wake and the answer travels in the shared cells.
//
// The lock is a ticket pair: a draw takes g_next_ticket with LDXR/STXR, and the holder alone
// advances g_now_serving with a plain load and an STLR.

#include <kickos/arch/arch.h>
#include <kickos/chip_limits.h>

#include "../common/gic.h"

#include <kickos/arch/doorbell_protocol.h>
#include <kickos/arch/klock_owner.h>

#include <kickos/sys/atomic.h>

#include <stddef.h>
#include <stdint.h>

// The doorbell half is built whenever something rings it: peers above one core, or an AMP
// node's peer nodes. The lock and the park are for the cores of one kernel.
#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)

extern "C"
{
    void kfault_terminate(void) __attribute__((noreturn));
#if KICKOS_BENCH && KICKOS_KERNEL_CORES > 1
    void kickos_bench_lock_draw(uint32_t retries, uint32_t queued);
#endif
}

namespace
{
#if KICKOS_KERNEL_CORES > 1
    // Separate lines: a draw takes g_next_ticket's line exclusive, which on a shared line
    // would invalidate it under every waiter loading g_now_serving. DDI 0487 M.b B2.12.5 asks
    // for a reservation granule between objects reached by exclusive accesses.
    alignas(KICKOS_DOORBELL_LINE) uint32_t g_next_ticket = 0;
    alignas(KICKOS_DOORBELL_LINE) uint32_t g_now_serving = 0;

    // Pairs with the STLR the holder releases with.
    uint32_t now_serving(void)
    {
        uint32_t v = 0;
        __asm volatile("ldar %w0, [%1]" : "=r"(v) : "r"(&g_now_serving) : "memory");
        return v;
    }

    // LDXR, not LDAXR: a drawn ticket grants nothing and so owes no acquire.
    //
    // Nothing in the retry interval may be a System register write, translation, cache or TLB
    // maintenance, exception generating or return instruction, indirect branch or BL between a
    // failing Store-Exclusive and the Load-Exclusive retry, or a PE without FEAT_LSE loses its
    // forward-progress guarantee (DDI 0487 M.b B2.12.5, condition 2).
#if KICKOS_BENCH
    uint32_t kernel_lock_draw(uint32_t& retries)
    {
        uint32_t tick = 0;
        uint32_t want = 0;
        uint32_t res = 0;
        uint32_t tries = 0;
        __asm volatile("1:     add     %w3, %w3, #1\n"
                       "       ldxr    %w0, [%4]\n"
                       "       add     %w1, %w0, #1\n"
                       "       stxr    %w2, %w1, [%4]\n"
                       "       cbnz    %w2, 1b\n"
                       : "=&r"(tick), "=&r"(want), "=&r"(res), "+r"(tries)
                       : "r"(&g_next_ticket)
                       : "memory");
        retries = tries - 1u;
        return tick;
    }
#else
    uint32_t kernel_lock_draw(void)
    {
        uint32_t tick = 0;
        uint32_t want = 0;
        uint32_t res = 0;
        __asm volatile("1:     ldxr    %w0, [%3]\n"
                       "       add     %w1, %w0, #1\n"
                       "       stxr    %w2, %w1, [%3]\n"
                       "       cbnz    %w2, 1b\n"
                       : "=&r"(tick), "=&r"(want), "=&r"(res)
                       : "r"(&g_next_ticket)
                       : "memory");
        return tick;
    }
#endif
#endif

    struct Part
    {
        static void clear(void)
        {
            kickos_armv8a_gic_doorbell_clear();
        }
        static void raise_self(void)
        {
            kickos_armv8a_gic_doorbell_send(1u << arch_cpu_id());
        }
        static bool inject_owed(void)
        {
            return false;
        }
        // WFI wakes on a pending interrupt whatever PSTATE.I says, and the window then takes it.
        static void sleep(void)
        {
            arch_idle_wait();
            arch_irq_window();
        }
        static void open(void)
        {
            arch_irq_window();
        }
        // Restores the bank doorbell_only narrowed and drops this core's doorbell pending state.
        static void before_start(void)
        {
            kickos_armv8a_gic_percore_init();
        }
    };
}

// A context synchronization event on this PE: until a PE takes one, instructions it already
// fetched may be re-executed with no bound (DDI 0487 M.b B2.7.4.2), and no operation makes one
// PE synchronize another. Spelled explicitly because the poll runs the service from inside a
// spin, which enters no exception. tests/static/check_doorbell_isb.sh reads it out of the image.
kickos::doorbell::Fenced kickos::doorbell::service_fence(Observed)
{
    __asm volatile("isb" ::: "memory");
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
    kickos_armv8a_gic_doorbell_send(cores);
}

#if KICKOS_KERNEL_CORES > 1
// A raise over a pending SGI is idempotent.
void arch_ipi_raise(uint32_t cores)
{
    kickos_armv8a_gic_doorbell_send(cores);
}
#endif

// A full barrier: the pairing it serves is a store then a load on both sides, the one
// direction release and acquire leave free. Its body is asserted out of the linked image
// (tests/static/check_ipi_fence.sh).
void arch_ipi_fence(void)
{
    __asm volatile("dmb ish" ::: "memory");
}

// One poke and one wait over `peers`; the effect is the ISB every serviced core runs.
#if KICKOS_NUM_CORES > 1
void kickos_arm64_instruction_side_rendezvous(uint32_t peers)
{
    arch_ipi_send(peers);
    arch_ipi_wait(peers);
}
#endif

#if KICKOS_KERNEL_CORES > 1
void arch_kernel_lock(void)
{
#if KICKOS_BENCH
    uint32_t retries = 0;
    uint32_t const ticket = kernel_lock_draw(retries);
    kickos_bench_lock_draw(retries, ticket - now_serving());
#else
    uint32_t const ticket = kernel_lock_draw();
#endif
    kickos::doorbell::await_turn([ticket] { return now_serving() == ticket; });
    kickos::klock::owner_take();
}

// STLR pairs with the LDAR in now_serving.
void arch_kernel_unlock(void)
{
    kickos::klock::owner_drop();
    uint32_t v = 0;
    __asm volatile("ldr  %w0, [%1]\n"
                   "add  %w0, %w0, #1\n"
                   "stlr %w0, [%1]"
                   : "=&r"(v)
                   : "r"(&g_now_serving)
                   : "memory");
}
#endif

#if KICKOS_NUM_CORES > 1
void kickos_armv8a_doorbell_park(void)
{
    // Keeps kickos_isr_timer out of a core that reaches no scheduler.
    kickos_armv8a_gic_doorbell_only();
#if KICKOS_AMP_NODE
    // Drains what was sent before this core could be poked: a sender that found it unseatable
    // published without raising. Read masked, behind the barrier pairing with the sender's.
    arch_ipi_fence();
    kickos_amp_node_service();
#endif
    kickos::doorbell::park<Part>();
}
#endif

}

#endif
