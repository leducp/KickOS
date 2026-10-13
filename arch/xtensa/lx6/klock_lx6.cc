// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What the cross-core doorbell and the kernel lock coupled to it cost on the Xtensa LX6: the
// trigger's clear, the ticket draw's conditional store, and the secondary park. The protocol
// over them, the service body included, is in arch/common/doorbell_protocol.cc.
//
// The rendezvous is shared memory. The trigger register reports nothing back to a sender and is
// not even per target core, so the answer travels in the shared cells. The trigger register is
// not such a cell and need not be: two cores only ever set it. The lock is a ticket pair: a
// draw takes g_next_ticket with S32C1I, and the holder alone advances g_now_serving with L32I
// and S32RI. Both counters are written only inside this file.
//
// Nothing clears the trigger in hardware, so a sender setting it while a receiver clears it can
// erase the wake. That costs a SPURIOUS ENTRY and never a lost request, since the set then
// lands after the clear and the LEVEL input re-asserts. The service body must stay idempotent.

#include <kickos/arch/arch.h>
#include <kickos/arch/doorbell_protocol.h>
#include <kickos/arch/klock_owner.h>
#include <kickos/arch/lx6_doorbell.h>

#if KICKOS_NUM_CORES > 1

#include <kickos/sys/atomic.h>

#include <stdint.h>

extern "C" void kfault_terminate(void) __attribute__((noreturn));
#if KICKOS_BENCH && KICKOS_KERNEL_CORES > 1
// Declared rather than included: this TU is below <kickos/bench.h>.
extern "C" void kickos_bench_lock_draw(uint32_t retries, uint32_t queued);
#endif

namespace
{
    struct Part
    {
        static void clear(void)
        {
            kickos_lx6_doorbell_clear();
        }
        static void raise_self(void)
        {
            kickos_lx6_doorbell_send(1u << arch_cpu_id());
        }
        static bool inject_owed(void)
        {
#if KICKOS_KERNEL_CORES > 1
            return kickos_lx6_inject_owed() != 0;
#else
            return false;
#endif
        }
        // WAITI takes PS.INTLEVEL from its immediate: the unmask and the sleep in one
        // instruction. Inline, as tests/static/check_lx6_park_mask.sh reads it in the park.
        static __attribute__((always_inline)) void sleep(void)
        {
            __asm volatile("waiti 0" ::: "memory");
        }
        // To level 0 whatever level the park was entered at, which arch_irq_restore would not
        // give.
        static __attribute__((always_inline)) void open(void)
        {
            uint32_t ps = 0;
            __asm volatile("rsil %0, 0" : "=a"(ps) : : "memory");
            (void)ps;
        }
        static void before_start(void)
        {
        }
    };

#if KICKOS_KERNEL_CORES > 1
    // Adjacent and unpadded: KICKOS_DOORBELL_LINE is 4 on this part because no cache covers the
    // internal SRAM kernel state lives in, so there is no line for two counters to share and
    // padding would only cost bytes on the part with the least RAM. Kernel state, so esp32.ld's
    // internal-SRAM range assert is what keeps both where S32C1I's RCW transaction excludes the
    // other CPU.
    uint32_t g_next_ticket = 0;
    uint32_t g_now_serving = 0;

    // S32C1I against SCOMPARE1 (Conditional Store Option). It excludes only while ATOMCTL
    // selects the RCW bus transaction, which kickos_lx6_init seats, refusing a core that cannot
    // confirm it.
    //
    // The load stays inside the loop: S32C1I usually returns the current memory value, but the
    // ISA summary (p.120) allows a few implementations to return the bitwise NOT of SCOMPARE1
    // instead when the store is not done, so a hoisted load is not portable across them.
    //
    // The instruction plays the role of both acquire and release on its own (4.3.13.5, p.122),
    // but it sits on the DRAW here, which grants nothing: the acquire is owed by the wait.
    uint32_t kernel_lock_draw(uint32_t& retries)
    {
        uint32_t cur = 0u;
        uint32_t want = 0u;
#if KICKOS_BENCH
        uint32_t tries = 0u;
        __asm volatile("1:  addi %[n], %[n], 1\n\t"
                       "    l32ai %[cur], %[addr], 0\n\t"
                       "    wsr.scompare1 %[cur]\n\t"
                       "    addi %[want], %[cur], 1\n\t"
                       "    s32c1i %[want], %[addr], 0\n\t"
                       "    bne %[want], %[cur], 1b"
                       : [cur] "=&a"(cur), [want] "=&a"(want), [n] "+a"(tries)
                       : [addr] "a"(&g_next_ticket)
                       : "memory");
        retries = tries - 1u;
#else
        __asm volatile("1:  l32ai %[cur], %[addr], 0\n\t"
                       "    wsr.scompare1 %[cur]\n\t"
                       "    addi %[want], %[cur], 1\n\t"
                       "    s32c1i %[want], %[addr], 0\n\t"
                       "    bne %[want], %[cur], 1b"
                       : [cur] "=&a"(cur), [want] "=&a"(want)
                       : [addr] "a"(&g_next_ticket)
                       : "memory");
        (void)retries;
#endif
        return cur;
    }

    // L32AI, the Multiprocessor Synchronization Option's load-acquire (ISA summary 4.3.12,
    // Table 49, p.117), and the acquire half of the lock.
    uint32_t now_serving(void)
    {
        uint32_t v = 0u;
        __asm volatile("l32ai %[v], %[addr], 0"
                       : [v] "=a"(v)
                       : [addr] "a"(&g_now_serving)
                       : "memory");
        return v;
    }
#endif
}

extern "C"
{

int kickos_lx6_doorbell_pending(void)
{
    if (kickos::doorbell::pending())
    {
        return 1;
    }
    return 0;
}

} // extern "C"

// No barrier: this part has no translation to invalidate and no cache over the memory both cores
// fetch from.
kickos::doorbell::Fenced kickos::doorbell::service_fence(Observed)
{
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
    kickos_lx6_doorbell_send(cores);
}

#if KICKOS_KERNEL_CORES > 1
// A core's own trigger is routed to its own doorbell input, and the input is level, so a raise
// made under this core's mask stands until it unmasks.
void arch_ipi_raise(uint32_t cores)
{
    kickos_lx6_doorbell_send(cores);
}
#endif

#if KICKOS_KERNEL_CORES > 1
void arch_kernel_lock(void)
{
    uint32_t retries = 0u;
    uint32_t const ticket = kernel_lock_draw(retries);
#if KICKOS_BENCH
    kickos_bench_lock_draw(retries, ticket - now_serving());
#endif
    kickos::doorbell::await_turn([ticket] { return now_serving() == ticket; });
    kickos::klock::owner_take();
}

// S32RI, the ISA's store-release (Multiprocessor Synchronization Option, ISA summary 4.3.12,
// p.115), which the Conditional Store Option above has as a PREREQUISITE (4.3.13, p.118) and so
// is present wherever S32C1I is. Deliberately not the `memw` plus plain store the compiler
// emits for a release elsewhere in this image: it pairs visibly with the L32AI the wait reads
// the turn with.
void arch_kernel_unlock(void)
{
    kickos::klock::owner_drop();
    uint32_t v = 0u;
    __asm volatile("l32i  %[v], %[addr], 0\n\t"
                   "addi  %[v], %[v], 1\n\t"
                   "s32ri %[v], %[addr], 0"
                   : [v] "=&a"(v)
                   : [addr] "a"(&g_now_serving)
                   : "memory");
}
#endif

// Nothing to narrow on the way in: kickos_lx6_init left this core's INTENABLE carrying the
// doorbell alone.
void kickos_lx6_doorbell_park(void)
{
    kickos::doorbell::park<Part>();
}

}

#endif
