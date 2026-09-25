// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "tx_smp_seam.h"

#include <kickos/arch/arch.h>
#include <kickos/console_tx.h>
#include <kickos/irq.h>
#include <kickos/instance.h>
#include <kickos/irq_route.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace
{
    static_assert(KICKOS_KERNEL_CORES == 2 and KICKOS_NUM_CORES == 2,
                  "these arms speak as core one against core zero; a different count makes "
                  "every exclusion assertion below describe another machine, and at one core "
                  "arch_cpu_id below is a macro this file may not define");

    // How long a rendezvous waits for its peer before declaring the hand-off broken; kept
    // well under the ctest timeout so a blown budget fails with a verdict instead of hanging.
    constexpr auto HANDOFF_BUDGET = std::chrono::seconds(10);

    // Guards the fixture's own records alone.
    std::mutex g_records;
    std::string g_wire;
    uint32_t g_pushes[KICKOS_KERNEL_CORES] = {};
    bool g_outside = false;

    std::atomic<int> g_lock_owner{-1};
    // A std::mutex and not an atomic flag: an atomic read-modify-write is a libatomic call
    // on two of the fleet's backends and this corpus refuses one (tests/static/atomic_rmw).
    std::mutex g_lock_word;
    std::atomic<bool> g_contended{false};

    std::atomic<uint32_t> g_hold_core{0xFFFFFFFFu};
    void (*g_hold_fn)(void) = nullptr;
    std::atomic<bool> g_hold_fired{false};

    thread_local int g_mask_depth = 0;
    char g_storage[4096];

    int mock_slot_free(void)
    {
        return 1;
    }

    void mock_push(uint8_t b)
    {
        uint32_t const me = consoletxsmp::g_core;
        {
            std::lock_guard<std::mutex> guard(g_records);
            g_wire.push_back(static_cast<char>(b));
            g_pushes[me]++;
            int const owner = g_lock_owner.load();
            if (owner >= 0 and static_cast<uint32_t>(owner) != me)
            {
                g_outside = true;
            }
        }
        // Outside the record lock: a seated function parks until its peer moves, and the
        // peer's own push has to reach these records while it does.
        if (g_hold_core.load() == me and g_hold_fn != nullptr)
        {
            void (*fn)(void) = g_hold_fn;
            g_hold_fn = nullptr;
            g_hold_core.store(0xFFFFFFFFu);
            g_hold_fired.store(true);
            fn();
        }
    }

    void mock_irq_enable(void)
    {
    }

    void mock_irq_disable(void)
    {
    }

    console_tx_backend const g_backend = {mock_slot_free, mock_push, mock_irq_enable,
                                          mock_irq_disable};
}

namespace consoletxsmp
{
    thread_local uint32_t g_core = 0;

    void reset(uint32_t ring_size)
    {
        std::lock_guard<std::mutex> guard(g_records);
        g_wire.clear();
        for (uint32_t i = 0; i < KICKOS_KERNEL_CORES; i++)
        {
            g_pushes[i] = 0;
        }
        g_outside = false;
        g_lock_owner.store(-1);
        g_contended.store(false);
        g_hold_core.store(0xFFFFFFFFu);
        g_hold_fn = nullptr;
        g_hold_fired.store(false);
        console_tx_init(&g_backend, g_storage, ring_size, 3);
    }

    std::string wire()
    {
        std::lock_guard<std::mutex> guard(g_records);
        return g_wire;
    }

    uint32_t pushes_by(uint32_t core)
    {
        if (core >= KICKOS_KERNEL_CORES)
        {
            return 0;
        }
        std::lock_guard<std::mutex> guard(g_records);
        return g_pushes[core];
    }

    bool push_outside_the_lock()
    {
        std::lock_guard<std::mutex> guard(g_records);
        return g_outside;
    }

    bool lock_contended()
    {
        return g_contended.load();
    }

    void hold_at_push(uint32_t core, void (*fn)(void))
    {
        g_hold_fn = fn;
        g_hold_fired.store(false);
        g_hold_core.store(core);
    }

    bool hold_fired()
    {
        return g_hold_fired.load();
    }

    bool wait_for(bool (*pred)(void))
    {
        auto const deadline = std::chrono::steady_clock::now() + HANDOFF_BUDGET;
        while (not pred())
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                return false;
            }
            std::this_thread::yield();
        }
        return true;
    }
}

extern "C"
{

// At one core, arch_cpu_id is a macro folding to a literal and no other source may define
// it; the assert above pins which arm this translation unit is on.
#if KICKOS_NUM_CORES > 1
uint32_t arch_cpu_id(void)
{
    return consoletxsmp::g_core;
}
#endif

// Per-thread, so a producer's mask reaches only the core it speaks as. A shared depth here
// would make one core's critical section exclude the other's and hide the overlap these
// arms exist to read.
arch_irq_state_t arch_irq_save(void)
{
    arch_irq_state_t const prior = static_cast<arch_irq_state_t>(g_mask_depth);
    g_mask_depth++;
    return prior;
}

void arch_irq_restore(arch_irq_state_t state)
{
    g_mask_depth = static_cast<int>(state);
}

int arch_in_isr(void)
{
    return 0;
}

// Excludes and nothing else, as arch/xtensa/lx6/klock_lx6.cc does. The owner is published
// under the claim and withdrawn before the release, so a reader inside a push names the core
// whose critical section it landed in.
void arch_kernel_lock(void)
{
    uint32_t const me = arch_cpu_id();
    while (true)
    {
        if (g_lock_word.try_lock())
        {
            g_lock_owner.store(static_cast<int>(me));
            return;
        }
        g_contended.store(true);
        std::this_thread::yield();
    }
}

void arch_kernel_unlock(void)
{
    g_lock_owner.store(-1);
    g_lock_word.unlock();
}

#if KICKOS_DEBUG
int arch_kernel_lock_held(void)
{
    if (g_lock_owner.load() < 0)
    {
        return 0;
    }
    return 1;
}
#endif

void arch_ipi_raise(uint32_t)
{
}

void arch_ipi_send(uint32_t)
{
}

void arch_ipi_wait(uint32_t)
{
}

// Unmasked on every backend, so these bytes reach the wire without a lock owner to name.
void arch_console_write_sync(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        mock_push(static_cast<uint8_t>(buf[i]));
    }
}

void arch_irq_mask(int)
{
}

void arch_irq_unmask(int)
{
}

void arch_irq_clear_pending(int)
{
}

console_tx_backend const* arch_console_tx_backend(char**, uint32_t*, int*)
{
    return nullptr; // the fixture arms the ring through console_tx_init
}

// Both ownership reads are pinned kernel-owned: these arms measure the ring.
int console_owner_is_kernel(void)
{
    return 1;
}

int console_chip_writable(void)
{
    return 1;
}

void console_chip_writer_enter(void)
{
}

void console_chip_writer_leave(void)
{
}

// Stubs console.cc's version; the insert reaches it only for the unarmed ring, where there
// is nothing to interleave with, so these suites need only the symbol.
void console_write_line_sync(char const*, size_t)
{
}
}

namespace kickos
{
    // klock.cc's release reads the scheduler's flush row.
    namespace detail
    {
        constinit InstanceLocal<Kernel> g_instance;
    }

    void kpanic(char const*) __attribute__((noreturn));
    void kpanic(char const*)
    {
        __builtin_trap();
    }

    bool irq_attach(int, IrqHandler, void*)
    {
        return true;
    }

    void irq_detach(int)
    {
    }

    // The lock's release publishes what the scheduler staged, and this gate stages nothing.
    void sched_flush_owed(uint32_t)
    {
    }

    void irq_line_op(int line, LineOp op)
    {
        switch (op)
        {
            case LineOp::MASK:
            {
                arch_irq_mask(line);
                break;
            }
            case LineOp::UNMASK:
            {
                arch_irq_unmask(line);
                break;
            }
            case LineOp::CLEAR:
            {
                arch_irq_clear_pending(line);
                break;
            }
        }
    }

    void irq_line_op_local(int line, LineOp op)
    {
        irq_line_op(line, op);
    }
}
