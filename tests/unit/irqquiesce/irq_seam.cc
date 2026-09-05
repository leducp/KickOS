// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The WHOLE seam between kernel/irq/irq.cc and the rest of the image, re-derived with
//
//   nm --undefined-only <the object> | comm -23 - <its defined symbols>
//
// at two kernel cores.
//
// arch_ipi_send and arch_ipi_wait carry a real request/answer pair, and arch_kernel_lock
// services a pending doorbell while it spins, as arch/arm64/armv8a/klock_armv8a.cc does. Both
// halves are load-bearing for the threaded arms: a teardown that polls a peer's cell must
// COMPLETE its rendezvous against a peer wedged in the acquire loop, or the arm hangs instead
// of reddening.
//
// A doorbell's far side runs no dispatch entry and so advances no epoch: a teardown that
// returned because the doorbell answered has proved nothing about the epochs.

#include "irq_seam.h"

#include <stdio.h>
#include <stdlib.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include <kickos/irq_route.h>
#include <kickos/arch/arch.h>
#include <kickos/cap.h>
#include <kickos/instance.h>
#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sync.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

namespace kickos
{
    namespace detail
    {
        constinit InstanceLocal<Kernel> g_instance;
    }

    namespace irqfix
    {
        namespace
        {
            constexpr unsigned SEAM_TRACE_MAX = 4096;
            SeamEvent g_trace[SEAM_TRACE_MAX];
            unsigned g_trace_n = 0;
            // A LEAF LOCK, never held while anything else is taken: the threaded arms append
            // from two threads at once, and a lost append would silently drop an event a
            // verdict is read off.
            std::mutex g_trace_mu;

            void note(SeamOp op, uint32_t arg)
            {
                std::lock_guard<std::mutex> guard(g_trace_mu);
                if (g_trace_n < SEAM_TRACE_MAX)
                {
                    g_trace[g_trace_n].op = op;
                    g_trace[g_trace_n].arg = arg;
                }
                g_trace_n++;
            }

            // Written by the sending core alone, so a load and a store carry it.
            std::atomic<unsigned> g_ipi_sends[KICKOS_NUM_CORES];

            // The gate. Plain loads and stores: an RMW spelling is refused in a tracked source,
            // and the one-shot needs none, since only the held core clears the armed line.
            std::atomic<int> g_hold_line{-1};
            std::atomic<bool> g_hold_reached{false};
            std::atomic<bool> g_hold_timed_out{false};
            std::atomic<bool> g_hold_released{false};
            constexpr auto MASK_HOLD_BUDGET = std::chrono::seconds(10);

            void hold_here_if_armed(int line)
            {
                if (g_hold_line.load() != line)
                {
                    return;
                }
                g_hold_line.store(-1);
                g_hold_reached.store(true);
                auto const deadline = std::chrono::steady_clock::now() + MASK_HOLD_BUDGET;
                while (not g_hold_released.load())
                {
                    if (std::chrono::steady_clock::now() > deadline)
                    {
                        g_hold_timed_out.store(true);
                        return;
                    }
                    std::this_thread::yield();
                }
            }
        }

        thread_local uint32_t g_core = 0;
        int g_line_core = -1;
        uint32_t g_pinned_mask = 0;
        unsigned g_probe_calls = 0;
        void* g_probe_arg = nullptr;
        void (*g_probe_action)() = nullptr;

        std::atomic<bool> g_lock_blocked{false};
        std::atomic<unsigned> g_posts{0};

        void reset()
        {
            g_trace_n = 0;
            g_core = 0;
            g_line_core = -1;
            g_pinned_mask = 0;
            g_probe_calls = 0;
            g_probe_arg = nullptr;
            g_probe_action = nullptr;
            g_lock_blocked.store(false);
            g_posts.store(0);
            g_hold_line.store(-1);
            g_hold_reached.store(false);
            g_hold_timed_out.store(false);
            g_hold_released.store(false);
            for (uint32_t core = 0; core < KICKOS_NUM_CORES; core++)
            {
                g_ipi_sends[core].store(0);
            }
        }

        // Only the interrupt half of the Kernel: the struct holds an Atomic and so is not
        // assignable.
        void reset_kernel()
        {
            Kernel& k = kernel();
            for (int i = 0; i < k.irq_bindings.capacity(); i++)
            {
                k.irq_refs[i] = 0;
                if (k.irq_bindings.live(i))
                {
                    k.irq_bindings.free(k.irq_bindings.handle_for(i));
                }
            }
            irq_init();
        }

        SeamEvent event(unsigned i)
        {
            unsigned const n = g_trace_n;
            if (i >= n or i >= SEAM_TRACE_MAX)
            {
                SeamEvent none = {OP_MASK, 0xFFFFFFFFu};
                return none;
            }
            return g_trace[i];
        }

        unsigned count_of(SeamOp op)
        {
            unsigned n = 0;
            unsigned const end = g_trace_n;
            for (unsigned i = 0; i < end and i < SEAM_TRACE_MAX; i++)
            {
                if (g_trace[i].op == op)
                {
                    n++;
                }
            }
            return n;
        }

        int first_of(SeamOp op)
        {
            unsigned const end = g_trace_n;
            for (unsigned i = 0; i < end and i < SEAM_TRACE_MAX; i++)
            {
                if (g_trace[i].op == op)
                {
                    return static_cast<int>(i);
                }
            }
            return -1;
        }

        int last_line_op(int line)
        {
            int op = -1;
            unsigned const end = g_trace_n;
            for (unsigned i = 0; i < end and i < SEAM_TRACE_MAX; i++)
            {
                if (g_trace[i].op != OP_MASK and g_trace[i].op != OP_UNMASK)
                {
                    continue;
                }
                if (g_trace[i].arg == static_cast<uint32_t>(line))
                {
                    op = static_cast<int>(g_trace[i].op);
                }
            }
            return op;
        }

        void hold_next_mask(int line)
        {
            g_hold_reached.store(false);
            g_hold_timed_out.store(false);
            g_hold_released.store(false);
            g_hold_line.store(line);
        }

        bool mask_hold_reached() { return g_hold_reached.load(); }
        bool mask_hold_timed_out() { return g_hold_timed_out.load(); }
        void release_mask_hold() { g_hold_released.store(true); }
    }

    // --- the kernel-side stubs ------------------------------------------------------------

    void kpanic(char const* msg)
    {
        printf("kickos: panic: %s\n", msg);
        fflush(stdout);
        abort();
    }

    void sem_init(Semaphore* s, int count)
    {
        s->count = count;
        s->waiters = List();
    }

    // TAKES IrqLock FIRST, exactly as kernel/sync/sync.cc does. This is the whole of the cycle
    // an arm reproduces: reached from inside a dispatch entry, it cannot get past this line
    // while a teardown on another core holds the lock, so the calling core answers doorbells
    // but never lowers its own dispatch epoch.
    bool sem_post(Semaphore*)
    {
        IrqLock lock;
        irqfix::g_posts.store(irqfix::g_posts.load() + 1u);
        return true;
    }

    void wq_block(List&, WaitKind, void*)
    {
    }

    void wq_confirm_resume(Thread*, uint32_t)
    {
    }

    // Enough of the placement layer for irq_claim's routed-core pin: the grant is the whole
    // machine, and the pin is recorded, there being no run queue here to move a thread between.
    uint32_t task_core_set(Task const*)
    {
        return ~0u;
    }

    namespace sched
    {
        void set_affinity(Thread*, uint32_t mask)
        {
            irqfix::g_pinned_mask = mask;
        }
    }

    // Enough of the capability layer for irq_claim and its undo: one handle, resolving to the
    // one binding the arm installed.
    namespace
    {
        void* g_installed_obj = nullptr;
        int g_installed_handle = -1;
        unsigned g_closes = 0;
    }

    int cap_install(Thread*, int obj, CapType, uint8_t, uint32_t* out)
    {
        g_installed_handle = obj;
        g_installed_obj = kernel().irq_bindings.resolve(obj);
        *out = 1u;
        return 0;
    }

    void* cap_resolve_e(Thread*, uint32_t, CapType, uint8_t, int* err)
    {
        if (g_installed_obj == nullptr)
        {
            *err = KOS_EBADF;
            return nullptr;
        }
        return g_installed_obj;
    }

    // EMPTIES THE SLOT BEFORE IT DROPS THE REFERENCE, as kernel/syscall/cap.cc does. That order
    // is what makes a refused release unrecoverable: once this returns, nothing names the object
    // any more, so a reference the drop puts back has no owner left to drop it again.
    int handle_close(Thread*, uint32_t)
    {
        g_closes++;
        int const obj = g_installed_handle;
        g_installed_obj = nullptr;
        g_installed_handle = -1;
        if (obj >= 0)
        {
            irq_ref_drop(obj, false);
        }
        return 0;
    }

    namespace irqfix
    {
        unsigned closes()
        {
            return g_closes;
        }

        int installed_handle()
        {
            return g_installed_handle;
        }

        void reset_caps()
        {
            g_installed_obj = nullptr;
            g_installed_handle = -1;
            g_closes = 0;
        }

        unsigned ipi_sends(uint32_t core)
        {
            if (core >= KICKOS_NUM_CORES)
            {
                return 0;
            }
            return g_ipi_sends[core].load();
        }

        void bump_ipi_sends(uint32_t core)
        {
            if (core < KICKOS_NUM_CORES)
            {
                g_ipi_sends[core].store(g_ipi_sends[core].load() + 1u);
            }
        }
    }
}

static_assert(KICKOS_NUM_CORES > 1 and KICKOS_KERNEL_CORES > 1,
              "this seam answers the multi-core arm of every declaration below; at one core "
              "the doorbell and the core identity are macros and these are redefinitions");

namespace
{
    // The doorbell's request and answer cells, one row per core. Each row is written by its
    // own core alone, so a load and a store carry it.
    std::atomic<uint32_t> g_request[KICKOS_NUM_CORES][KICKOS_NUM_CORES];
    std::atomic<uint32_t> g_answer[KICKOS_NUM_CORES][KICKOS_NUM_CORES];

    // The lock word. A mutex rather than a spun flag: an exchange or a compare-exchange is a
    // read-modify-write, which no tracked source in this tree may spell.
    std::mutex g_lock_word;

    // Rounds arch_ipi_wait spends on one peer before it gives up on an answer.
    constexpr unsigned DOORBELL_WAIT_SPINS = 100000u;

    bool doorbell_pending(uint32_t me)
    {
        for (uint32_t from = 0; from < KICKOS_NUM_CORES; from++)
        {
            if (g_request[from][me].load() != g_answer[me][from].load())
            {
                return true;
            }
        }
        return false;
    }

    // Answers every outstanding request aimed at this core, running no dispatch entry and so
    // touching no epoch.
    void doorbell_poll(uint32_t me)
    {
        if (not doorbell_pending(me))
        {
            return;
        }
        for (uint32_t from = 0; from < KICKOS_NUM_CORES; from++)
        {
            g_answer[me][from].store(g_request[from][me].load());
        }
    }
}

extern "C"
{

// GUARDED BY THE SEAM'S OWN CONDITION: at one core arch_cpu_id is a macro folding to a
// literal and no source in the tree may define it. The assert above pins which arm this
// translation unit is on.
#if KICKOS_NUM_CORES > 1
uint32_t arch_cpu_id(void)
{
    return kickos::irqfix::g_core;
}
#endif

arch_irq_state_t arch_irq_save(void)
{
    return 0;
}

void arch_irq_restore(arch_irq_state_t)
{
}

void arch_irq_mask(int line)
{
    // BEFORE the record: a gated dispatch's mask must land after whatever the other core did
    // while it was held, or the trace cannot say which came last.
    kickos::irqfix::hold_here_if_armed(line);
    kickos::irqfix::note(kickos::irqfix::OP_MASK, static_cast<uint32_t>(line));
}

void arch_irq_unmask(int line)
{
    kickos::irqfix::note(kickos::irqfix::OP_UNMASK, static_cast<uint32_t>(line));
}

void arch_irq_clear_pending(int line)
{
    kickos::irqfix::note(kickos::irqfix::OP_CLEAR, static_cast<uint32_t>(line));
}

// -1 is no constraint, so an arm that leaves kickos::irqfix::g_line_core alone sees irq_claim
// place nothing.
int arch_irq_line_core(int)
{
    return kickos::irqfix::g_line_core;
}

void arch_ipi_send(uint32_t cores)
{
    kickos::irqfix::note(kickos::irqfix::OP_IPI_SEND, cores);
    uint32_t const me = arch_cpu_id();
    kickos::irqfix::bump_ipi_sends(me);
    for (uint32_t to = 0; to < KICKOS_NUM_CORES; to++)
    {
        if ((cores & (1u << to)) != 0)
        {
            g_request[me][to].store(g_request[me][to].load() + 1u);
        }
    }
    if ((cores & (1u << me)) != 0)
    {
        doorbell_poll(me);
    }
}

void arch_ipi_wait(uint32_t cores)
{
    kickos::irqfix::note(kickos::irqfix::OP_IPI_WAIT, cores);
    uint32_t const me = arch_cpu_id();
    for (uint32_t to = 0; to < KICKOS_NUM_CORES; to++)
    {
        if ((cores & (1u << to)) == 0 or to == me)
        {
            continue;
        }
        uint32_t const asked = g_request[me][to].load();
        // BOUNDED, and it returns rather than faulting when the budget blows: a core an arm
        // names but runs no thread for answers nothing, and an unbounded spin would turn every
        // such arm into a hang, which reports no verdict. A core that IS running answers from
        // its acquire loop long inside this budget.
        unsigned spins = 0;
        while (g_answer[to][me].load() != asked and spins < DOORBELL_WAIT_SPINS)
        {
            spins++;
            doorbell_poll(me);
            std::this_thread::yield();
        }
    }
}

// Excludes and nothing else, as arch/arm64/armv8a/klock_armv8a.cc does, and SERVICES A PENDING
// DOORBELL WHILE IT SPINS: without that an initiator holding the lock would wait on this core,
// which is waiting on the initiator.
void arch_kernel_lock(void)
{
    uint32_t const me = arch_cpu_id();
    while (true)
    {
        if (g_lock_word.try_lock())
        {
            return;
        }
        kickos::irqfix::g_lock_blocked.store(true);
        doorbell_poll(me);
        std::this_thread::yield();
    }
}

void arch_kernel_unlock(void)
{
    g_lock_word.unlock();
}

// The raise the real klock.cc restores an owed reschedule with.
void arch_ipi_resched_self(void)
{
}

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
// ktrace.h is header-inline and reaches BOTH of these from kernel/irq/irq.cc.
uint32_t arch_trace_now(void)
{
    return 0;
}

int kickos_rtt_write_record_ch1(uint8_t const*, size_t)
{
    return 1; // accepted: a 0 here would count a drop on every record
}
#endif

}

namespace kickos
{
    namespace irqfix
    {
        // Unlocking a mutex from a thread that does not hold it is undefined, so the lock
        // word is left to the arms, which leave it balanced.
        void reset_lock()
        {
            g_lock_blocked.store(false);
            for (uint32_t a = 0; a < KICKOS_NUM_CORES; a++)
            {
                for (uint32_t b = 0; b < KICKOS_NUM_CORES; b++)
                {
                    g_request[a][b].store(0);
                    g_answer[a][b].store(0);
                }
            }
        }
    }
}

namespace kickos
{
    // One core, so every line is local. Forwarded to the arch stubs in this file, which is
    // what keeps each arm's recorded trace unchanged.
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
