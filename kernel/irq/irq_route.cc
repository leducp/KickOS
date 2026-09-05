// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// THE SOLE DECIDER of which core performs a logical line's delivery gating, and the only file
// in the kernel layer that may call arch_irq_mask, arch_irq_unmask or arch_irq_clear_pending.
// tests/static/check_irq_line_op_sole.sh enforces that. The words those three seam members
// read-modify-write are IMAGE-WIDE, so a caller on another core loses a mask or a latched raise
// with no fault anywhere; freeze N3 buys one kernel lock with that pin.
//
// A server holding CAP_WAIT is PLACED on its line's core and a grant that cannot reach it is
// refused; a passer-by is never moved, so its TOUCH is routed instead
// (docs/design-multicore.md N3 and section 8).
//
// COMPILED ON THREE BACKENDS AND FIRING ON ONE. Only the lx6 defines arch_irq_line_core;
// armv8a and rv64imac take the lone-TU fallback answering -1, so line_op_ask is unreachable
// there, and the link cannot drop it, arch_irq_line_core being an extern. Do not delete it as
// dead: a backend that starts routing needs no kernel change.

#include <kickos/irq_route.h>

#include <kickos/arch/arch.h>
#include <kickos/config.h>
#include <kickos/debug.h> // KICKOS_DEBUG_ASSERT
#include <kickos/instance.h> // kickos_kernel_core()
#include <kickos/irqlock.h>

#if KICKOS_KERNEL_CORES > 1
#include <kickos/sys/atomic.h>
#include <stdint.h>
#endif

namespace kickos
{
    namespace
    {
        // Inlined for the red zone: reached from the doorbell service body, so a frame here is
        // charged to every chain that can spin on the kernel lock (check_trap_redzone.sh,
        // class PREEMPT).
        __attribute__((always_inline)) inline void line_op_here(int line, LineOp op)
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

#if KICKOS_KERNEL_CORES > 1
        using Seq = Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE>;

        // One row per asking core, written by that core alone. `line` and `op` are published
        // BEFORE the sequence that makes them readable, and read only after it has moved.
        struct Ask
        {
            Seq seq;
            int32_t line;
            uint8_t op;
        };
        struct AskRow
        {
            Ask to[KICKOS_KERNEL_CORES];
        };

        // g_ask[i].to[t]: what core i has asked core t to do, and how many times.
        AskRow g_ask[KICKOS_KERNEL_CORES] = {};
        // g_ran[t].to[i]: how far core t has performed core i's asks.
        AskRow g_ran[KICKOS_KERNEL_CORES] = {};

        void line_op_ask(uint32_t owner, int line, LineOp op)
        {
            uint32_t const me = kickos_kernel_core();
            Ask& a = g_ask[me].to[owner];
            // Payload before the sequence, whose release store the far side acquires.
            a.line = static_cast<int32_t>(line);
            a.op = static_cast<uint8_t>(op);
            a.seq = a.seq.load() + 1u;
            // THE ANSWER IS THE COMPLETION: there is no second cell, and the precondition is
            // that every backend's doorbell service body drains AFTER its request snapshot and
            // BEFORE it stores the answer. A drain ahead of the snapshot answers work it never
            // did. tests/static/check_route_service_order.sh asserts it in all three bodies.
            //
            // One slot per ordered pair suffices under that ordering: an answer for ask N means
            // N was drained, so N+1 cannot be published while N is outstanding, and nothing
            // re-enters on this slot (the remote path is refused in ISR context, and the drain
            // performs the op locally without nesting an ask).
            arch_ipi_send(1u << owner);
            arch_ipi_wait(1u << owner);
        }
#endif
    }

    // For a caller that is the routed core by construction, which is every ISR-context caller.
    //
    // A SEPARATE ENTRY, NOT A BRANCH: check_trap_redzone.sh walks the CALLGRAPH, and a runtime
    // `if (arch_in_isr())` is invisible to it. Merging the two entries puts arch_ipi_wait's
    // panic tail on the interrupt path and overruns its red zone.
    void irq_line_op_local(int line, LineOp op)
    {
        line_op_here(line, op);
    }

    // The one entry for every caller that is NOT the routed core by construction. Callers name
    // the line and the operation and never the core.
    void irq_line_op(int line, LineOp op)
    {
#if KICKOS_KERNEL_CORES > 1
        int const owner = arch_irq_line_core(line);
        // -1 means no constraint: a controller raising every line on every core has no owner.
        //
        // BOUNDED HERE, where `owner` is both shifted into a core mask and used to index the
        // mailbox, each sized KICKOS_KERNEL_CORES. KICKOS_NUM_CORES is not the bound: under AMP
        // a kernel drives fewer cores than the image has, and a line routed outside the
        // kernel's own set cannot be asked at all.
        if (owner >= static_cast<int>(KICKOS_KERNEL_CORES))
        {
            KICKOS_DEBUG_ASSERT(owner < static_cast<int>(KICKOS_KERNEL_CORES));
            line_op_here(line, op);
            return;
        }
        if (owner >= 0 and static_cast<uint32_t>(owner) != kickos_kernel_core())
        {
            // A rendezvous entered from a handler would block an interrupt on a peer.
            if (arch_in_isr() == 0)
            {
                line_op_ask(static_cast<uint32_t>(owner), line, op);
                return;
            }
            // The op is still performed after the assertion: a release build that dropped it
            // would turn a wrong-core touch into a MISSING one, which is worse.
            KICKOS_DEBUG_ASSERT(arch_in_isr() == 0);
        }
#endif
        line_op_here(line, op);
    }

#if KICKOS_KERNEL_CORES > 1
    // Reached from a backend's doorbell SERVICE BODY and nowhere else. Moving it to
    // klock_resched_ask's cell looks like a cleanup and deadlocks (freeze N2): that cell is
    // drained only when the dispatch enters the scheduler, while a core spinning for the lock
    // the initiator holds runs the service body alone.
    //
    // The op takes no kernel lock, N2's requirement, because arch.h declares these three seam
    // members SELF-BRACKETED. A member that stops being so breaks this routing.
    //
    // Called between the request snapshot and the answer store; see line_op_ask.
    extern "C" void kickos_irq_route_service(void)
    {
        uint32_t const me = kickos_kernel_core();
        for (uint32_t from = 0; from < KICKOS_KERNEL_CORES; from++)
        {
            uint32_t const asked = g_ask[from].to[me].seq.load();
            if (asked == g_ran[me].to[from].seq.load())
            {
                continue;
            }
            // Read AFTER the sequence's acquire load above, which is what makes the two fields
            // the ones the asking core published with it.
            int const line = static_cast<int>(g_ask[from].to[me].line);
            LineOp const op = static_cast<LineOp>(g_ask[from].to[me].op);
            line_op_here(line, op);
            g_ran[me].to[from].seq = asked;
        }
    }
#endif
}
