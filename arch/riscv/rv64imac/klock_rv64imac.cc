// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The rv64imac cross-core doorbell and ticket kernel lock: the CLINT raise, sip.SSIP, the
// service's translation fence, and the secondary park. The protocol over them is in
// arch/common/doorbell_protocol.cc.
//
// The rendezvous cells are shared memory: a CLINT msip word carries the wake and nothing back.
// A draw takes g_next_ticket with AMOADD.W, and the holder alone advances g_now_serving.
//
// SSIP carries both a peer's raise and this hart's own device-line injection, so the cell and
// not the raise says a service is owed (kickos_rv64_doorbell_pending).

#include <kickos/arch/arch.h>
#include <kickos/arch/percpu.h>
#include <kickos/arch/doorbell_protocol.h>
#include <kickos/arch/klock_owner.h>
#include <kickos/arch/rv64_doorbell.h>

#if KICKOS_NUM_CORES > 1

#include <kickos/sys/atomic.h>

#include <stddef.h>
#include <stdint.h>

extern "C" void kfault_terminate(void) __attribute__((noreturn));
extern "C" void kickos_rv64_init(void);
#if KICKOS_BENCH && KICKOS_KERNEL_CORES > 1
// Declared, not included: this TU is below <kickos/bench.h>.
extern "C" void kickos_bench_lock_draw(uint32_t retries, uint32_t queued);
#endif

namespace
{
    // Nonzero once hart i has reached the park. Written by that hart alone.
    kickos::doorbell::PartCell g_online[KICKOS_NUM_CORES] = {};

    // A parked secondary drops sie.STIE: a timer trap there would enter kickos_isr_timer with
    // no scheduler to serve.
    constexpr uint64_t SIE_STIE = 1ull << 5;

    constexpr uint64_t SIP_SSIP = 1ull << 1;

    // A hart servicing the doorbell outside its handler owes this call.
    void doorbell_clear(void)
    {
        __asm volatile("csrc sip, %0" ::"r"(SIP_SSIP) : "memory");
    }

    // A self-raise needs no trip through the machine-mode trampoline a peer's raise takes.
    void doorbell_raise_self(void)
    {
        __asm volatile("csrs sip, %0" ::"r"(SIP_SSIP) : "memory");
    }

    struct Part
    {
        static void clear(void)
        {
            doorbell_clear();
        }
        static void raise_self(void)
        {
            doorbell_raise_self();
        }
        static bool inject_owed(void)
        {
            return kickos_rv64_inject_owed() != 0;
        }
        // WFI wakes on an interrupt pending in sie whatever sstatus.SIE says, and the window
        // then takes it.
        static void sleep(void)
        {
            arch_idle_wait();
            arch_irq_window();
        }
        static void open(void)
        {
            arch_irq_window();
        }
        // The timer goes back: this hart is about to own a scheduler that arms deadlines.
        static void before_start(void)
        {
            __asm volatile("csrs sie, %0" ::"r"(SIE_STIE) : "memory");
        }
    };

#if KICKOS_KERNEL_CORES > 1
    // Separate lines so a draw's write does not invalidate the line every waiter loads
    // g_now_serving from.
    alignas(KICKOS_DOORBELL_LINE) uint32_t g_next_ticket = 0;
    alignas(KICKOS_DOORBELL_LINE) uint32_t g_now_serving = 0;

    // No ordering bits: a drawn ticket grants nothing and publishes nothing.
    uint32_t kernel_lock_draw(void)
    {
        uint32_t tick = 0;
        __asm volatile("amoadd.w %0, %1, (%2)"
                       : "=r"(tick)
                       : "r"(1u), "r"(&g_next_ticket)
                       : "memory");
        return tick;
    }

    uint32_t now_serving(void)
    {
        uint32_t v = 0;
        __asm volatile("lw %0, 0(%1)" : "=r"(v) : "r"(&g_now_serving) : "memory");
        return v;
    }
#endif
}

extern "C"
{

int kickos_rv64_doorbell_pending(void)
{
    if (kickos::doorbell::pending())
    {
        return 1;
    }
    return 0;
}

} // extern "C"

// SFENCE.VMA with rs1 = rs2 = x0 is global, so one execution after observing any outstanding
// request covers all of them.
//
// A hart must run SFENCE.VMA itself: the ISA gives no way to perform it for another hart.
// FENCE.I is absent (no Zifencei in this board's ISA baseline).
kickos::doorbell::Fenced kickos::doorbell::service_fence(Observed)
{
    __asm volatile("sfence.vma zero, zero" ::: "memory");
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
    kickos_rv64_doorbell_send(cores);
}

#if KICKOS_KERNEL_CORES > 1
void arch_ipi_raise(uint32_t cores)
{
    uint32_t const self = 1u << arch_cpu_id();
    kickos_rv64_doorbell_send(cores & ~self);
    if ((cores & self) != 0)
    {
        doorbell_raise_self();
    }
}
#endif

// A FULL barrier: the pairing it serves is a store then a load on both sides, the one direction
// release and acquire leave free. Reached through a plain call no callgraph gate follows, so
// its body is asserted out of the linked image (tests/static/check_ipi_fence.sh).
void arch_ipi_fence(void)
{
    __asm volatile("fence rw, rw" ::: "memory");
}

// Makes every peer run its own SFENCE.VMA, there being no broadcast form. FENCE.I is absent
// from this board's ISA baseline and is not covered.
void kickos_rv64_translation_rendezvous(uint32_t peers)
{
    arch_ipi_send(peers);
    arch_ipi_wait(peers);
}

#if KICKOS_KERNEL_CORES > 1
void arch_kernel_lock(void)
{
    uint32_t const ticket = kernel_lock_draw();
#if KICKOS_BENCH
    // amoadd.w cannot retry, so retries is always zero.
    kickos_bench_lock_draw(0u, ticket - now_serving());
#endif
    kickos::doorbell::await_turn([ticket] { return now_serving() == ticket; });
    // Acquire half; no Zalasr load-acquire in this baseline.
    __asm volatile("fence r, rw" ::: "memory");
    kickos::klock::owner_take();
}

// Fence plus plain store is the release: it pairs with the fence the wait leaves on, so
// everything done under the lock publishes before the next ticket is served.
void arch_kernel_unlock(void)
{
    kickos::klock::owner_drop();
    uint32_t v = 0;
    __asm volatile("lw     %0, 0(%1)\n"
                   "addi   %0, %0, 1\n"
                   "fence  rw, w\n"
                   "sw     %0, 0(%1)"
                   : "=&r"(v)
                   : "r"(&g_now_serving)
                   : "memory");
}
#endif

void kickos_rv64_doorbell_park(void)
{
    // stimecmp's reset value is not architecturally all-ones, so the timer is disarmed
    // explicitly.
    arch_timer_disarm();
    __asm volatile("csrc sie, %0" ::"r"(SIE_STIE) : "memory");
    kickos::doorbell::park<Part>();
}

uint32_t kickos_rv64_core_online_read(uint32_t id)
{
    if (id >= KICKOS_NUM_CORES)
    {
        return 0;
    }
    return g_online[id].v.load();
}

// A released secondary's first supervisor code. kickos_rv64_init is per-hart and seats this
// hart's identity.
void kickos_rv64_secondary_entry(void)
{
    arch_timer_disarm();
    kickos_rv64_init();

    uint32_t const id = arch_cpu_id();
    if (id != 0 and id < KICKOS_NUM_CORES)
    {
        g_online[id].v = 1u;
        kickos_rv64_doorbell_park();
    }
    while (true)
    {
        __asm volatile("wfi");
    }
}

}

#endif
